<?php

class searchlight_backend_sphinx extends searchlight_backend {
  var $view;
  var $sphinx;
  var $query;
  var $params;
  var $ordinals;

  /**
   * Constructor.
   */
  function __construct(&$view) {
    module_load_include('php', 'sphinx_views', 'sphinxapi');
    $this->view   = $view;
    $this->query  = $view->query;
    $this->sphinx = new SphinxClient();
    $this->params = $view->sphinx_views;
    $this->sphinx->SetServer('localhost', 9312);
  }

  /**
   * Execute a Sphinx query and provide its results to the View.
   */
  function execute_view() {
    // Skip Views' query execution - we will do it ourselves.
    $this->view->executed = TRUE;

    // Killswitch.
    if (!empty($this->params['abort'])) {
      return;
    }

    // Views query token replacements.
    $replacements = module_invoke_all('views_query_substitutions', $this->view);

    // @TODO do these based on params.
    $this->sphinx->SetMatchMode($this->params['matchMode']);

    // Set filters.
    foreach ($this->query->where as $group => $where) {
      // Copy where args and do token replacements.
      // These will be passed by reference to set_filter() so it can eat
      // arguments progressively from clauses.
      $where_args = !empty($where['args']) ? $where['args'] : array();
      if (is_array($where_args)) {
        foreach ($where_args as $id => $arg) {
          $where_args[$id] = str_replace(array_keys($replacements), $replacements, $arg);
        }
      }
      foreach ($where['clauses'] as $key => $clause) {
        // Query Views token replacements.
        $clause = str_replace(array_keys($replacements), $replacements, $clause);
        $this->set_filter($clause, $where_args);
      }
    }

    // Set sorts.
    $this->set_sort($this->query->orderby);

    // Set pager. Initial call limits the Sphinx query.
    $this->set_pager();

    // Execute the Sphinx query.
    $result = $this->execute_sphinx();
    if (!empty($result['matches'])) {
      $this->view->total_rows = $result['total_found'] - $this->view->pager['offset'];
      $this->set_pager();

      // Clear out normal field, sort, where portions of the query that
      // have been taken care of by Sphinx.
      $this->query->orderby = array();
      $this->query->where = array();

      // @TODO: do this with placeholders, args.
      $ids = implode(',', array_keys($result['matches']));
      $this->query->add_where(0, "{$this->view->base_table}.{$this->view->base_field} IN ({$ids})");

      // Build query, args
      $main = $this->query->query();
      $args = $this->query->get_where_args();

      // Replace tokens in the query, args.
      $main = str_replace(array_keys($replacements), $replacements, $main);
      if (is_array($args)) {
        foreach ($args as $id => $arg) {
          $args[$id] = str_replace(array_keys($replacements), $replacements, $arg);
        }
      }

      // Execute query and build result set.
      $dataset = array_fill_keys(array_keys($result['matches']), FALSE);
      $views_result = db_query($main, $args);
      while ($row = db_fetch_object($views_result)) {
        if (isset($dataset[$row->{$this->view->base_field}])) {
          $dataset[$row->{$this->view->base_field}] = $row;
        }
      }
      $dataset = array_filter($dataset);
      $dataset = array_values($dataset);
      $this->view->result = $dataset;
    }
  }

  /**
   * Execute a Sphinx query and return the results. Uses the current sphinx
   * client object, params.
   */
  function execute_sphinx() {
    return $this->sphinx->Query($this->params['query'], sphinx_views_get_datasource_id($this->view->base_table));
  }

  /**
   * Execute the current Sphinx search with a groupby on the given facet.
   */
  function execute_facet($attribute, $count = 20) {
    $this->sphinx->SetGroupBy($attribute, SPH_GROUPBY_ATTR, '@count desc');
    $this->sphinx->SetLimits(0, $count);
    return $this->execute_sphinx();
  }

  /**
   * Build a facet results ready for theming from a Sphinx query result
   * returned by ->execute_facet().
   */
  function build_facet($attribute, $result = array()) {
    $items = array();
    $ids = array();

    // Collect facet information from result set.
    if (!empty($result['matches'])) {
      foreach ($result['matches'] as $row) {
        if (isset($row['attrs']['@groupby'], $row['attrs']['@count'])) {
          if ($attribute['type'] === 'sql_attr_str2ordinal') {
            $id = $this->get_ordinal($attribute['name'], $row['attrs']['@groupby']);
          }
          else {
            $id = $row['attrs']['@groupby'];
          }
          $count = $row['attrs']['@count'];
          $ids[] = $id;
          $items[$id] = array(
            'id' => $id,
            'count' => $count,
            'attribute' => $attribute,
          );
        }
      }
    }

    // Render facets.
    return $this->render_facet($attribute, $items);
  }

  /**
   * Render the facet using its view handler.
   */
  function render_facet($attribute, &$items) {
    $rendered = array();
    if ($attribute['type'] === 'sql_attr_multi') {
      $view = views_get_view($attribute['view']);
      $view->set_display($attribute['display']);
      $view->build();

      $options = $view->execute_display();
      if (!empty($options['mva_label_field']) && !empty($view->field[$options['mva_label_field']])) {
        $handler = $view->field[$options['mva_label_field']];
        $handler_fields = array($handler->real_field);

        // Claim: The table of the label field contains both the MVA field and
        // the label field itself (and are sometimes the same thing).
        $ids = array_keys($items);
        $placeholders = is_numeric($ids[0]) ? db_placeholders($ids, 'int') : db_placeholders($ids, 'text');
        $query =
          "SELECT {$attribute['field']} AS {$attribute['name']}, {$handler->real_field} AS {$handler->field_alias}
          FROM {{$handler->table}} {$handler->table}
          WHERE {$handler->table}.{$attribute['field']} IN ($placeholders)";
        $result = db_query($query, $ids);

        // Now render each row.
        while ($row = db_fetch_object($result)) {
          $items[$row->{$attribute['name']}]['rendered'] = $handler->render($row);
        }
        $label = $handler->label();
      }
    }
    else {
      $view = sphinx_views_get_view($this->view->base_table);
      $view->build();

      // Detect which field handler generated this attribute. Real pain.
      if (isset($view->field[$attribute['field']])) {
        $handler = $view->field[$attribute['field']];
      }
      else {
        foreach ($view->field as $field) {
          if ($field->real_field === $attribute['field'] || (!empty($field->additional_fields) && in_array($attribute['field'], $field->additional_fields))) {
            $handler = $field;
            break;
          }
        }
      }

      // Fake some rows so we can use the pre_render method.
      if (!empty($handler)) {
        $rows = array();
        foreach (array_keys($items) as $id) {
          $row = new stdClass();
          $row->{$attribute['name']} = $id;
          $rows[] = $row;
        }
        $handler->pre_render($rows);

        // Now render each row.
        foreach ($rows as $row) {
          $items[$row->{$attribute['name']}]['rendered'] = $handler->render($row);
        }
        $label = $handler->label();
      }
    }
    return array('label' => $label, 'items' => $items);
  }

  /**
   * Get the ordinal integer for a given string, or vice versa.
   */
  function get_ordinal($attribute_name, $value, $reset = FALSE) {
    if (!isset($this->ordinals) || $reset) {
      $cid = "sphinx_ordinals_{$this->view->base_table}";
      if (!$reset && $cache = cache_get($cid)) {
        $this->ordinals = $cache->data;
      }
      if ($reset || !isset($this->ordinals)) {
        $mapping = $this->get_ordinals();
        if (!empty($mapping)) {
          cache_set($cid, $mapping);
        }
        $this->ordinals = $mapping;
      }
    }
    if (is_numeric($value)) {
      return array_search($value, $this->ordinals[$attribute_name]);
    }
    else {
      return isset($this->ordinals[$attribute_name][$value]) ? $this->ordinals[$attribute_name][$value] : FALSE;
    }
  }

  /**
   * Get an ordinal mapping for this datasource.
   * As this is a very expensive operation, its results are cached and
   * used in the ->get_ordinal() method. You should only call this method
   * if you know what you are doing.
   */
  function get_ordinals() {
    $datasource = sphinx_views_get_datasource($this->view->base_table);

    // Retrieve all ordinal attributes.
    $attributes = array();
    if (!empty($datasource['attributes'])) {
      foreach ($datasource['attributes'] as $field => $attribute) {
        if ($attribute['type'] === 'sql_attr_str2ordinal') {
          $attributes[$field] = $attribute;
        }
      }
    }

    // If there are attributes with ordinals, generate a mapping.
    // We only handle string ordinals for sets of less than 1000 (for now).
    $ordinals = array();
    if (!empty($attributes)) {
      $sphinx = new SphinxClient();
      $sphinx->SetLimits(0, 1000);

      foreach ($attributes as $attribute) {
        // Dispatch a Sphinx query to retrieve the ordinal.
        $sphinx->SetGroupBy($attribute['name'], SPH_GROUPBY_ATTR, '@count desc');
        $sphinx->SetGroupDistinct($attribute['name']);
        $result = $sphinx->Query(NULL, sphinx_views_get_datasource_id($this->view->base_table));

        if (count($result['matches']) < 1000) {
          // Dispatch a Views query to retrieve the corresponding string.
          $ids = implode(',', array_keys($result['matches']));
          $view = sphinx_views_get_view($this->view->base_table);
          $view->build();
          $view->set_items_per_page(0);
          $view->query->where = array();
          $view->query->add_where(0, "{$view->base_table}.{$view->base_field} IN ({$ids})");
          $view->build_info['query'] = $view->query->query();
          $view->build_info['query_args'] = $view->query->get_where_args();
          $view->execute();
          foreach ($view->result as $row) {
            $id = $row->{$view->base_field};
            if (isset($result['matches'][$id], $row->{$attribute['name']})) {
              $ordinals[$attribute['name']][$row->{$attribute['name']}] = $result['matches'][$id]['attrs'][$attribute['name']];
            }
          }
        }
      }
    }
    return $ordinals;
  }

  /**
   * Set a Sphinx filter from a SQL WHERE clause.
   */
  function set_filter($clause, &$where_args) {
    $datasource = sphinx_views_get_datasource($this->view->base_table);
    $split = preg_split('/[ ]([<>=!]*)|(IN)|(NOT IN)/i', trim($clause), NULL, PREG_SPLIT_NO_EMPTY | PREG_SPLIT_DELIM_CAPTURE);
    if (count($split) >= 2) {
      $field = trim($split[0]);
      $operator = trim($split[1]);
      if (!empty($datasource['attributes'][$field])) {
        $attribute = $datasource['attributes'][$field];

        // Get the arguments for this where clause.
        $args = array();
        $argnum = substr_count($split[2], '%');
        if ($argnum === 0) {
          $args[] = $split[2];
        }
        else if (count($where_args) >= $argnum) {
          while ($argnum > 0) {
            $args[] = array_shift($where_args);
            $argnum--;
          }
        }
        // @TODO: between range filters (e.g. >= AND <=)
        switch (strtoupper($operator)) {
          case '=':
            $this->SetFilter($attribute['name'], $args, FALSE);
            return TRUE;
          case 'IN':
            $this->SetFilter($attribute['name'], $args, FALSE);
            return TRUE;
          case 'NOT IN':
            $this->SetFilter($attribute['name'], $args, TRUE);
            return TRUE;
          case '>':
            $max = 10000000000;
            $min = eval("return {$args[0]};");
            $this->sphinx->SetFilterRange($attribute['name'], $min, $max, TRUE);
            return TRUE;
          case '>=':
            $max = 10000000000;
            $min = eval("return {$args[0]};");
            $this->sphinx->SetFilterRange($attribute['name'], $min, $max, FALSE);
            return TRUE;
          case '<':
            $min = 0;
            $max = eval("return {$args[0]};");
            $this->sphinx->SetFilterRange($attribute['name'], $min, $max, TRUE);
            return TRUE;
          case '<=':
            $min = 0;
            $max = eval("return {$args[0]};");
            $this->sphinx->SetFilterRange($attribute['name'], $min, $max, FALSE);
            return TRUE;
        }
      }
    }
    return FALSE;
  }

  /**
  * Set global pager vars based on the view.
  */
  function set_pager() {
    if (!isset($this->view->total_rows)) {
      $pagers = isset($_GET['page']) ? explode(',', $_GET['page']) : array();
      if (!empty($this->view->pager['items_per_page'])) {
        $this->sphinx->SetLimits(($pagers[$this->view->pager['element']] * $this->view->pager['items_per_page']) + $this->view->pager['offset'], $this->view->pager['items_per_page']);
      }
      else {
        $this->sphinx->SetLimits(($pagers[$this->view->pager['element']] * $this->view->pager['items_per_page']) + $this->view->pager['offset'], 1000);
      }
    }
    else if (!empty($this->view->pager['use_pager'])) {
      // dump information about what we already know into the globals
      global $pager_page_array, $pager_total, $pager_total_items;
      // total rows in query
      $pager_total_items[$this->view->pager['element']] = $this->view->total_rows;
      // total pages
      $pager_total[$this->view->pager['element']] = ceil($pager_total_items[$this->view->pager['element']] / $this->view->pager['items_per_page']);
      // What page was requested:
      $pager_page_array = isset($_GET['page']) ? explode(',', $_GET['page']) : array();

      // If the requested page was within range. $this->view->pager['current_page']
      // defaults to 0 so we don't need to set it in an out-of-range condition.
      if (!empty($pager_page_array[$this->view->pager['element']])) {
        $page = intval($pager_page_array[$this->view->pager['element']]);
        if ($page > 0 && $page < $pager_total[$this->view->pager['element']]) {
          $this->view->pager['current_page'] = $page;
        }
      }
      $pager_page_array[$this->view->pager['element']] = $this->view->pager['current_page'];
    }
  }

  /**
   * Set Sphinx sort method from ORDER BY clauses.
   */
  function set_sort($orderby = array()) {
    $sort = array();
    $datasource = sphinx_views_get_datasource($this->view->base_table);
    foreach ($orderby as $clause) {
      $split = explode(' ', $clause);
      if (count($split) <= 2) {
        $field = $split[0];
        $direction = isset($split[1]) ? $split[1] : 'ASC';
        if ($field === 'sphinx_weight') {
          $sort[] = "@weight {$direction}";
          continue;
        }
        foreach ($datasource['attributes'] as $attribute) {
          if ($attribute['name'] === $field) {
            $sort[] = "{$field} {$direction}";
            continue;
          }
        }
      }
    }
    // @TODO: Add support for SPH_SORT_TIME_SEGMENTS.
    if (!empty($sort)) {
      $this->sphinx->setSortMode(SPH_SORT_EXTENDED, implode(', ', $sort));
    }
    else {
      $this->sphinx->setSortMode(SPH_SORT_RELEVANCE);
    }
  }

  /**
   * Wrappers =================================================================
   */

  /**
   * Wrapper of $sphinx->SetFilter().
   * Convert any string values to ordinals before passing through.
   */
  function SetFilter($attribute, $values, $exclude = FALSE) {
    $ordinals = array();
    foreach ($values as $arg) {
      $arg = trim($arg);
      if (is_numeric($arg)) {
        $ordinals[] = $arg;
      }
      else {
        $ordinal = $this->get_ordinal($attribute, $arg);
        if (is_numeric($ordinal)) {
          $ordinals[] = $ordinal;
        }
      }
    }
    return $this->sphinx->SetFilter($attribute, $ordinals, $exclude);
  }
}