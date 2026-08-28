#!/usr/bin/env Rscript
# Visualize benchmark results from result/ directory using ggplot2.
#
# Reads *_debug.csv for runtime region boundary metrics and
# *.csv (non-debug) for execution/profiling/compilation times.
#
# Usage:
#   Rscript scripts/plot_results.R [OPTIONS]
#
# By default this plots the two headline metrics -- execution time and runtime
# region boundary hits -- normalized against the config's "normalize_ref"
# baseline, one plot per capacitor size.
#
# Options:
#   --result-dir DIR    Result directory (default: results/)
#   --absolute          Plot raw values instead of values normalized w.r.t. the
#                       config's "normalize_ref" baseline (or, if there is none,
#                       the first algorithm)
#   --all-metrics       Plot every metric, not just the two default ones
#   --output-dir DIR    Save plots to directory instead of showing
#   --benchmarks B,...  Comma-separated benchmark filter (default: all)
#   --metrics M,...     Comma-separated metric filter (overrides the default set)
#   --log-scale         Force log scale for execution_time and runtime_region_boundary_calls
#   --config FILE       Series definitions: which CSVs to read, plus their labels
#                       and styles (default: plot_config.json next to this script).
#                       Use plot_config_o0.json to add the -O0 execution-time
#                       baseline (apples-to-apples, since the passes run on -O0 IR).

script_args <- commandArgs(trailingOnly = FALSE)
script_file_arg <- script_args[grepl("^--file=", script_args)]
script_path <- if (length(script_file_arg) > 0) {
  sub("^--file=", "", script_file_arg[1])
} else {
  NA_character_
}
script_dir <- if (!is.na(script_path)) {
  dirname(normalizePath(script_path))
} else {
  "scripts"
}
source(file.path(script_dir, "plot_common.R"))

REQUIRED_ALGO_FOR_BENCHMARKS <- NULL

HAS_GGPATTERN <- requireNamespace("ggpattern", quietly = TRUE)
PDF_DEVICE <- "pdf"

# -- Metric definitions -------------------------------------------------------

METRICS <- list(
  region_boundaries = list(
    source = "debug", column = "region_boundaries",
    include_baselines = FALSE,
    ylabel = "# Static Boundaries",
    relative_ylabel = "Relative Boundaries",
    title = "Region Boundaries"
  ),
  runtime_region_boundary_calls = list(
    source = "debug", column = "runtime_region_boundary_calls",
    include_baselines = FALSE,
    ylabel = "# Boundary Calls",
    relative_ylabel = "Relative Hits",
    title = "Region Boundary Calls at Run-time"
  ),
  execution_time = list(
    source = "no-debug", column = "execution_time_us",
    include_baselines = TRUE,
    ylabel = expression("Execution Time (" * mu * "s)"),
    relative_ylabel = "Relative Time",
    title = "Execution Time"
  ),
  profiling_time = list(
    source = "no-debug", column = "profiling_time_ms",
    include_baselines = FALSE,
    ylabel = "Profiling Time (ms)",
    relative_ylabel = "Relative Profile Time",
    title = "Profiling Time"
  ),
  compilation_time = list(
    source = "no-debug", column = "compilation_time_ms",
    include_baselines = TRUE,
    ylabel = "Compilation Time (ms)",
    relative_ylabel = "Compilation Ratio",
    title = "Compilation Time"
  )
)

BENCHMARK_LABELS <- c(
  activity_recognition = "ar"
)

# -- Theme --------------------------------------------------------------------

theme_benchmark <- function() {
  theme_minimal(base_size = 15, base_family = "Helvetica") +
    theme(
      plot.title = element_blank(),
      plot.caption = element_text(color = "grey35", size = 10.5, hjust = 0,
                                  margin = margin(t = 4)),
      axis.title.x = element_text(size = AXIS_TITLE_SIZE,
                                  margin = margin(t = 7)),
      axis.title.y = element_text(size = AXIS_TITLE_SIZE,
                                  margin = margin(r = 7)),
      axis.text.x = element_text(size = 14, angle = 15, hjust = 1, vjust = 1),
      axis.text.y = element_text(size = 12.5),
      legend.position = "top",
      legend.title = element_blank(),
      legend.text = element_text(size = 13),
      legend.key.height = unit(LEGEND_KEY_HEIGHT_CM, "cm"),
      legend.key.width = unit(0.68, "cm"),
      legend.box.spacing = unit(0.01, "cm"),
      legend.margin = margin(0, 0, 0, 0),
      panel.grid.major.x = element_blank(),
      panel.grid.minor = element_blank(),
      panel.grid.major.y = element_line(color = "grey90", linewidth = 0.3),
      panel.border = element_rect(color = "grey80", fill = NA, linewidth = 0.4),
      plot.margin = margin(2, 2, 4, 2)
    )
}

# -- Data loading -------------------------------------------------------------

format_benchmark_labels <- function(benchmarks) {
  labels <- BENCHMARK_LABELS[benchmarks]
  labels[is.na(labels)] <- benchmarks[is.na(labels)]
  unname(labels)
}

parse_benchmark_cap <- function(benchmark) {
  # Split "name-cap" into (name, cap). E.g. "aes-5uF" -> ("aes", "5uF")
  parts <- str_match(benchmark, "^(.+)-(\\d+uF)$")
  if (is.na(parts[1, 1])) {
    return(tibble(name = benchmark, cap = NA_character_))
  }
  tibble(name = parts[1, 2], cap = parts[1, 3])
}

resolve_csv_path <- function(result_dir, spec, source) {
  explicit <- if (source == "debug") spec$debug_csv else spec$csv
  candidates <- if (!is.na(explicit)) {
    file.path(result_dir, explicit)
  } else if (source == "debug") {
    file.path(result_dir, paste0(spec$algo, c("_debug.csv", "-swbor.csv")))
  } else {
    file.path(result_dir, paste0(spec$algo, c(".csv", "-swbor-no-debug.csv")))
  }
  found <- candidates[file.exists(candidates)]
  if (length(found) > 0) found[1] else NA_character_
}

read_result_csv <- function(path) {
  if (is.na(path) || !file.exists(path) || file.info(path)$size == 0) {
    return(tibble())
  }

  df <- suppressWarnings(read_csv(path, show_col_types = FALSE))
  if (!"benchmark" %in% names(df)) {
    return(tibble())
  }

  df
}

load_algorithm_data <- function(result_dir, spec, source, column) {
  path <- resolve_csv_path(result_dir, spec, source)
  if (is.na(path)) return(tibble())

  df <- read_result_csv(path)
  if (nrow(df) == 0) return(tibble())

  df <- df %>% filter(!is.na(benchmark), benchmark != "")

  if (!column %in% names(df)) return(tibble())

  df %>%
    mutate(parsed = map(benchmark, parse_benchmark_cap)) %>%
    unnest(parsed) %>%
    filter(!is.na(cap)) %>%
    transmute(
      benchmark = name,
      cap = cap,
      algo = spec$algo,
      value = as.numeric(.data[[column]])
    ) %>%
    filter(!is.na(value))
}

read_baseline_csv <- function(result_dir, filename, algo, column) {
  path <- file.path(result_dir, filename)
  if (!file.exists(path)) return(tibble())

  df <- read_result_csv(path)
  if (nrow(df) == 0) return(tibble())

  df <- df %>% filter(!is.na(benchmark), benchmark != "")

  if (!column %in% names(df)) return(tibble())

  df %>%
    transmute(
      benchmark = benchmark,
      cap = NA_character_,
      algo = algo,
      value = as.numeric(.data[[column]])
    ) %>%
    filter(!is.na(value))
}

baseline_applies <- function(spec, metric_key) {
  metrics <- spec$metrics[[1]]
  length(metrics) == 0 || metric_key %in% metrics
}

load_baseline_data <- function(result_dir, baselines, metric_key, column) {
  if (nrow(baselines) == 0) return(tibble())

  map_dfr(seq_len(nrow(baselines)), function(i) {
    spec <- baselines[i, ]
    if (!baseline_applies(spec, metric_key)) return(tibble())
    read_baseline_csv(result_dir, spec$csv, spec$algo, column)
  })
}

discover_capacitors <- function(result_dir, algorithms) {
  caps <- character()
  for (i in seq_len(nrow(algorithms))) {
    for (source in c("debug", "no-debug")) {
      path <- resolve_csv_path(result_dir, algorithms[i, ], source)
      if (is.na(path)) next
      df <- read_result_csv(path)
      if (nrow(df) == 0) next
      bm <- df$benchmark[!is.na(df$benchmark) & df$benchmark != ""]
      parsed <- map_dfr(bm, parse_benchmark_cap)
      caps <- c(caps, parsed$cap[!is.na(parsed$cap)])
    }
  }
  caps <- unique(caps)
  # Sort numerically by uF value
  nums <- as.numeric(str_replace(caps, "uF$", ""))
  caps[order(nums)]
}

# -- Plotting -----------------------------------------------------------------

# Plotted unless --all-metrics or an explicit --metrics list says otherwise.
DEFAULT_METRICS <- c("execution_time", "runtime_region_boundary_calls")

LOG_SCALE_METRICS <- c("execution_time", "runtime_region_boundary_calls")
OUTLIER_RATIO_THRESHOLD <- 3.0
OUTLIER_HEADROOM <- 1.15
# Bar value labels: vertical text anchored at its own bar. Gap and text
# extent are fractions of the panel height (axis space), so they look the
# same on linear, pseudo-log, and log axes.
LABEL_SIZE <- 3.6
LABEL_HEIGHT_SCALE <- 3.95 / PLOT_HEIGHT_IN
LABEL_GAP <- 0.015 * LABEL_HEIGHT_SCALE
LABEL_EXTENT_BASE <- 0.01 * LABEL_HEIGHT_SCALE
LABEL_EXTENT_PER_CHAR <- 0.028 * LABEL_HEIGHT_SCALE
format_metric_value <- function(value, normalize) {
  if (normalize) {
    return(if (value >= 100) sprintf("%.0f", value)
           else if (value >= 10) sprintf("%.1f", value)
           else sprintf("%.2f", value))
  }

  if (value >= 1e6) {
    return(sprintf("%.1fM", value / 1e6))
  }
  if (value >= 1e3) {
    return(sprintf("%.1fK", value / 1e3))
  }
  sprintf("%.0f", value)
}

axis_labeler <- label_number(scale_cut = cut_short_scale())

compute_transformed_breaks <- function(values, include_zero) {
  pos_vals <- values[is.finite(values) & values > 0]
  if (length(pos_vals) == 0) {
    return(if (include_zero) 0 else numeric())
  }

  min_exp <- floor(log10(min(pos_vals)))
  max_exp <- floor(log10(max(pos_vals)))
  exp_seq <- seq(min_exp, max_exp)
  if (length(exp_seq) > 4) {
    idx <- unique(floor(seq(1, length(exp_seq), length.out = 4)))
    exp_seq <- exp_seq[idx]
  }

  breaks <- 10^exp_seq
  breaks <- breaks[breaks >= min(pos_vals) * 0.8 &
                     breaks <= max(pos_vals) * 1.05]
  if (length(breaks) == 0) {
    breaks <- c(min(pos_vals), max(pos_vals))
  }

  if (include_zero) {
    c(0, unique(breaks))
  } else {
    unique(breaks)
  }
}

compute_display_limit <- function(values) {
  pos_vals <- sort(values[is.finite(values) & values > 0], decreasing = TRUE)
  if (length(pos_vals) < 3) {
    return(NA_real_)
  }

  largest <- pos_vals[1]
  second <- pos_vals[2]
  tail <- unname(quantile(pos_vals, probs = 0.9, na.rm = TRUE))

  if (largest / second < OUTLIER_RATIO_THRESHOLD ||
      largest / tail < OUTLIER_RATIO_THRESHOLD / 1.5) {
    return(NA_real_)
  }

  max(second, tail) * OUTLIER_HEADROOM
}

plot_metric_for_cap <- function(cap, metric_key, metric_info,
                                algo_data, baseline_data, benchmarks, normalize,
                                log_scale, config) {
  alg_style <- config$style
  if (!is.null(REQUIRED_ALGO_FOR_BENCHMARKS)) {
    required_benchmarks <- algo_data %>%
      filter(
        cap == !!cap,
        algo == REQUIRED_ALGO_FOR_BENCHMARKS,
        !is.na(value)
      ) %>%
      distinct(benchmark) %>%
      pull(benchmark)

    benchmarks <- benchmarks[benchmarks %in% required_benchmarks]
  }

  if (length(benchmarks) == 0) return(NULL)

  include_baselines <- metric_info$include_baselines && nrow(baseline_data) > 0

  # Filter to this capacitor
  df <- algo_data %>% filter(cap == !!cap)

  if (include_baselines) {
    # Replicate the baselines for each benchmark (cap-independent)
    baselines_for_cap <- baseline_data %>%
      filter(benchmark %in% benchmarks) %>%
      mutate(cap = !!cap)
    df <- bind_rows(df, baselines_for_cap)
  }

  df <- df %>% filter(benchmark %in% benchmarks)
  if (nrow(df) == 0) return(NULL)

  force_log <- log_scale && metric_key %in% LOG_SCALE_METRICS

  # Determine normalization (geomean is computed after this step)
  norm_algo <- NULL
  if (normalize) {
    norm_algo <- if (include_baselines && !is.na(config$norm_ref)) {
      config$norm_ref
    } else {
      config$algorithms$algo[1]
    }
    base <- df %>%
      filter(algo == norm_algo) %>%
      select(benchmark, base_value = value)
    df <- df %>%
      left_join(base, by = "benchmark") %>%
      mutate(value = if_else(!is.na(base_value) & base_value != 0,
                             value / base_value, NA_real_)) %>%
      select(-base_value) %>%
      filter(algo != norm_algo)
  }

  # Compute geometric mean across benchmarks for each algorithm
  geomean_df <- df %>%
    filter(!is.na(value), value > 0) %>%
    group_by(algo) %>%
    summarise(value = exp(mean(log(value))), .groups = "drop") %>%
    mutate(benchmark = "geomean", cap = !!cap)
  df <- bind_rows(df, geomean_df)
  benchmarks_with_geomean <- c(benchmarks, "geomean")

  # Set factor levels for ordering (config series order)
  active_algos <- intersect(alg_style$algo, unique(df$algo))
  label_map <- setNames(
    alg_style$label[match(active_algos, alg_style$algo)],
    active_algos
  )
  color_map <- setNames(
    alg_style$color[match(active_algos, alg_style$algo)],
    label_map[active_algos]
  )

  df <- df %>%
    mutate(
      algo_label = factor(label_map[algo], levels = label_map),
      benchmark = factor(benchmark, levels = benchmarks_with_geomean)
    )

  display_limit <- compute_display_limit(df$value)
  df <- df %>%
    mutate(
      clipped = !is.na(display_limit) & value > display_limit,
      display_value = if_else(clipped, display_limit, value)
    )

  # Check if symlog is needed
  pos_vals <- df$display_value[!is.na(df$display_value) & df$display_value > 0]
  raw_pos_vals <- df$value[!is.na(df$value) & df$value > 0]
  use_symlog <- !force_log && length(raw_pos_vals) >= 2 &&
    max(raw_pos_vals) / min(raw_pos_vals) >= 100
  plot_df <- if (force_log) {
    df %>% filter(display_value > 0)
  } else {
    df
  }
  clipped_df <- plot_df %>% filter(clipped)

  # For log-scale plots, pre-transform values to log10 space and plot on a
  # linear axis. Drawing bars from y = 0 under coord_transform(log10) makes
  # ggpattern emit stripe segments over a huge clipped region, bloating the
  # PDF by megabytes. Label positions are still computed in data space on
  # plot_df_orig, then transformed.
  plot_df_orig <- plot_df
  log_t <- identity
  if (force_log) {
    log_y_lo <- min(pos_vals, na.rm = TRUE) * 0.88
    log_t <- function(v) log10(v / log_y_lo)
    plot_df <- plot_df %>% mutate(display_value = log_t(display_value))
  }

  y_sub <- NULL
  if (normalize && !is.null(norm_algo)) {
    y_label <- metric_info$relative_ylabel
    y_sub <- paste0("(to ", alg_style$label[alg_style$algo == norm_algo], ")")
  } else {
    y_label <- metric_info$ylabel
  }

  plot_notes <- character()
  if (!HAS_GGPATTERN) {
    plot_notes <- c(
      plot_notes,
      "Install ggpattern for textured fills in grayscale output."
    )
  }
  if (!is.na(display_limit)) {
    plot_notes <- c(
      plot_notes,
      paste0(
        "Bars above ", format_metric_value(display_limit, normalize),
        " are clipped for readability; labels show actual values."
      )
    )
  }

  bar_position <- position_dodge2(width = 0.8, preserve = "single",
                                  padding = 0.04)
  text_position <- position_dodge2(width = 0.8, preserve = "single",
                                   padding = 0.04)

  p <- ggplot(plot_df, aes(x = benchmark, y = display_value,
                           fill = algo_label, group = algo_label))
  if (HAS_GGPATTERN) {
    pattern_map <- setNames(
      alg_style$pattern[match(active_algos, alg_style$algo)],
      label_map[active_algos]
    )
    pattern_angle_map <- setNames(
      alg_style$pattern_angle[match(active_algos, alg_style$algo)],
      label_map[active_algos]
    )
    p <- p + ggpattern::geom_col_pattern(
      aes(pattern = algo_label, pattern_angle = algo_label),
      position = bar_position,
      width = 0.78,
      key_glyph = ggpattern::draw_key_polygon_pattern,
      color = "grey20",
      linewidth = 0.35,
      pattern_fill = PATTERN_FILL_COLOUR,
      pattern_colour = PATTERN_COLOUR,
      pattern_density = PATTERN_DENSITY,
      pattern_spacing = PATTERN_SPACING,
      pattern_units = PATTERN_UNITS,
      pattern_alpha = PATTERN_ALPHA,
      pattern_size = PATTERN_SIZE,
      pattern_key_scale_factor = 1.0
    ) +
      ggpattern::scale_pattern_manual(values = pattern_map, drop = FALSE) +
      ggpattern::scale_pattern_angle_manual(values = pattern_angle_map,
                                            drop = FALSE)
  } else {
    p <- p + geom_col(
      position = bar_position,
      width = 0.78,
      color = "grey20",
      linewidth = 0.35
    )
  }

  p <- p +
    scale_fill_manual(values = color_map, drop = FALSE) +
    scale_x_discrete(
      labels = format_benchmark_labels,
      expand = expansion(add = c(0.32, 0.24))
    ) +
    labs(
      x = NULL,
      y = y_label,
      caption = if (length(plot_notes) > 0) paste(plot_notes, collapse = "\n") else NULL
    )

  if (HAS_GGPATTERN) {
    p <- p + guides(
      fill = "none",
      pattern_angle = "none",
      pattern = guide_legend(
        nrow = 1,
        override.aes = list(
          pattern = unname(pattern_map),
          pattern_angle = unname(pattern_angle_map),
          fill = unname(color_map),
          color = "grey20",
          pattern_fill = PATTERN_FILL_COLOUR,
          pattern_colour = PATTERN_COLOUR,
          pattern_density = PATTERN_LEGEND_DENSITY,
          pattern_spacing = PATTERN_LEGEND_SPACING,
          pattern_units = PATTERN_UNITS,
          pattern_alpha = PATTERN_LEGEND_ALPHA,
          pattern_size = PATTERN_LEGEND_SIZE,
          pattern_key_scale_factor = PATTERN_LEGEND_SCALE
        )
      )
    )
  } else {
    p <- p + guides(fill = guide_legend(nrow = 1))
  }

  p <- p + theme_benchmark()

  # Axis-space mapping for label placement. Geoms take pre-transformed
  # values on force_log plots and data values otherwise.
  to_axis <- if (force_log) {
    log_t
  } else if (use_symlog) {
    pseudo_log_trans(base = 10)$transform
  } else {
    identity
  }
  from_axis <- if (use_symlog) pseudo_log_trans(base = 10)$inverse else identity

  label_df <- plot_df_orig %>%
    filter(!is.na(display_value), display_value > 0) %>%
    mutate(
      label = vapply(value, format_metric_value,
                     FUN.VALUE = character(1), normalize = normalize),
      label = if_else(clipped, paste0(label, " ^"), label)
    )
  bar_top <- max(to_axis(plot_df_orig$display_value), na.rm = TRUE)
  label_extent <- LABEL_EXTENT_BASE +
    LABEL_EXTENT_PER_CHAR * max(c(0, nchar(label_df$label)))
  y_hi_axis <- bar_top / (1 - LABEL_GAP - label_extent)
  label_df <- label_df %>%
    mutate(label_y = from_axis(to_axis(display_value) + LABEL_GAP * y_hi_axis))

  if (nrow(label_df) > 0) {
    p <- p + geom_text(
      data = label_df,
      aes(y = label_y, label = label,
          fontface = if_else(clipped, "bold", "plain")),
      position = text_position,
      angle = 90, hjust = 0, vjust = 0.5,
      size = LABEL_SIZE, color = "grey20"
    )
  }

  if (nrow(clipped_df) > 0) {
    clipped_df <- clipped_df %>%
      mutate(
        seg_y_start = display_value / OUTLIER_HEADROOM,
        seg_y_end = display_value
      )
    if (force_log) {
      clipped_df <- clipped_df %>%
        mutate(
          seg_y_start = log_t(seg_y_start),
          seg_y_end = log_t(seg_y_end)
        )
    }
    p <- p + geom_segment(
      data = clipped_df,
      aes(x = benchmark, xend = benchmark,
          y = seg_y_start, yend = seg_y_end,
          group = algo_label),
      inherit.aes = FALSE,
      position = text_position,
      linewidth = 0.35,
      color = "grey15",
      arrow = arrow(type = "closed", length = unit(0.07, "in"))
    )
  }

  # Normalization reference line
  if (normalize && !is.null(norm_algo)) {
    p <- p + geom_hline(yintercept = log_t(1.0), linetype = "dashed",
                        color = "grey50", linewidth = 0.5)
  }

  y_hi <- from_axis(y_hi_axis)

  if (force_log) {
    # Log-scaled bar chart drawn in pre-transformed log10 space on a linear
    # axis: bar polygons stay panel-sized, so ggpattern stripes don't bloat
    # the PDF. Breaks are placed at transformed positions with original
    # value labels, so the axis reads as a log axis.
    breaks_orig <- compute_transformed_breaks(pos_vals, include_zero = FALSE)
    p <- p +
      scale_y_continuous(
        labels = axis_labeler(breaks_orig),
        breaks = log_t(breaks_orig),
        expand = expansion(mult = c(0, 0.05))
      ) +
      coord_cartesian(ylim = c(0, y_hi), clip = "on")
  } else if (use_symlog) {
    p <- p + scale_y_continuous(
      trans = pseudo_log_trans(base = 10),
      labels = axis_labeler,
      breaks = compute_transformed_breaks(pos_vals, include_zero = TRUE),
      expand = expansion(mult = c(0, 0.03))
    ) +
      coord_cartesian(ylim = c(0, y_hi),
                      clip = "off")
  } else {
    p <- p + scale_y_continuous(
      expand = expansion(mult = c(0, 0.03)),
      labels = axis_labeler,
      n.breaks = 5
    ) +
      coord_cartesian(ylim = c(0, y_hi),
                      clip = "off")
  }

  if (is.null(y_sub)) p else stack_ylab(p, y_label, y_sub)
}

# -- Main ---------------------------------------------------------------------

main <- function() {
  args <- commandArgs(trailingOnly = TRUE)

  # Parse arguments
  result_dir <- "results"
  normalize <- TRUE
  log_scale <- FALSE
  config_path <- DEFAULT_CONFIG_PATH
  output_dir <- NULL
  filter_benchmarks <- NULL
  filter_metrics <- NULL
  all_metrics <- FALSE

  i <- 1
  while (i <= length(args)) {
    if (args[i] == "--result-dir" && i < length(args)) {
      result_dir <- args[i + 1]; i <- i + 2
    } else if (args[i] == "--absolute") {
      normalize <- FALSE; i <- i + 1
    } else if (args[i] == "--all-metrics") {
      all_metrics <- TRUE; i <- i + 1
    } else if (args[i] == "--log-scale") {
      log_scale <- TRUE; i <- i + 1
    } else if (args[i] == "--config" && i < length(args)) {
      config_path <- args[i + 1]; i <- i + 2
    } else if (args[i] == "--output-dir" && i < length(args)) {
      output_dir <- args[i + 1]; i <- i + 2
    } else if (args[i] == "--benchmarks" && i < length(args)) {
      filter_benchmarks <- str_split(args[i + 1], ",")[[1]]; i <- i + 2
    } else if (args[i] == "--metrics" && i < length(args)) {
      filter_metrics <- str_split(args[i + 1], ",")[[1]]; i <- i + 2
    } else {
      cat("Unknown argument:", args[i], "\n")
      i <- i + 1
    }
  }

  if (!dir.exists(result_dir)) {
    stop("Result directory does not exist: ", result_dir)
  }

  if (!HAS_GGPATTERN) {
    cat("Note: ggpattern not installed; using color-only fills.\n")
  }

  config <- load_plot_config(config_path)

  missing_baselines <- config$baselines$csv[
    !file.exists(file.path(result_dir, config$baselines$csv))
  ]
  if (length(missing_baselines) > 0) {
    cat("Note: baseline CSV not found in", result_dir, "- skipping:",
        paste(missing_baselines, collapse = ", "), "\n")
  }

  if (!is.null(output_dir)) {
    dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
  }

  metrics_to_plot <- if (!is.null(filter_metrics)) {
    filter_metrics[filter_metrics %in% names(METRICS)]
  } else if (all_metrics) {
    names(METRICS)
  } else {
    DEFAULT_METRICS
  }

  # Discover benchmarks
  all_benchmarks <- character()
  for (i in seq_len(nrow(config$algorithms))) {
    for (source in c("debug", "no-debug")) {
      path <- resolve_csv_path(result_dir, config$algorithms[i, ], source)
      if (is.na(path)) next
      df <- read_result_csv(path)
      if (nrow(df) == 0) next
      bm <- df$benchmark[!is.na(df$benchmark) & df$benchmark != ""]
      parsed <- map_dfr(bm, parse_benchmark_cap)
      all_benchmarks <- c(all_benchmarks, parsed$name[!is.na(parsed$name)])
    }
  }
  for (baseline_csv in config$baselines$csv) {
    path <- file.path(result_dir, baseline_csv)
    if (!file.exists(path)) next
    df <- read_result_csv(path)
    if (nrow(df) == 0) next
    all_benchmarks <- c(all_benchmarks, df$benchmark[!is.na(df$benchmark)])
  }
  all_benchmarks <- unique(all_benchmarks)

  if (!is.null(filter_benchmarks)) {
    benchmarks <- filter_benchmarks[filter_benchmarks %in% all_benchmarks]
  } else {
    benchmarks <- sort(all_benchmarks)
  }

  if (length(benchmarks) == 0) stop("No benchmarks found.")

  capacitors <- discover_capacitors(result_dir, config$algorithms)

  cat("Benchmarks:", paste(benchmarks, collapse = ", "), "\n")
  cat("Metrics:", paste(metrics_to_plot, collapse = ", "), "\n")
  cat("Capacitors:", paste(capacitors, collapse = ", "), "\n")

  for (metric_key in metrics_to_plot) {
    metric_info <- METRICS[[metric_key]]
    source <- metric_info$source
    column <- metric_info$column

    algo_data <- map_dfr(seq_len(nrow(config$algorithms)), function(i) {
      load_algorithm_data(result_dir, config$algorithms[i, ], source, column)
    })

    baseline_data <- if (metric_info$include_baselines) {
      load_baseline_data(result_dir, config$baselines, metric_key, column)
    } else {
      tibble()
    }

    for (cap in capacitors) {
      p <- plot_metric_for_cap(
        cap, metric_key, metric_info,
        algo_data, baseline_data, benchmarks, normalize, log_scale, config
      )
      if (is.null(p)) next

      if (!is.null(output_dir)) {
        norm_suffix <- if (normalize) "_normalized" else ""
        log_suffix <- if (log_scale && metric_key %in% LOG_SCALE_METRICS) "_log" else ""
        filename <- paste0(metric_key, "_", cap, norm_suffix, log_suffix, ".pdf")
        filepath <- file.path(output_dir, filename)

        n_bm <- length(benchmarks) + 1  # +1 for geomean
        w <- max(6.4, n_bm * 0.9 + 1.4)

        save_plot <- function() {
          ggsave(filepath, p, width = w, height = PLOT_HEIGHT_IN,
                 device = PDF_DEVICE)
        }
        if (HAS_GGPATTERN && log_scale && metric_key %in% LOG_SCALE_METRICS) {
          # ggpattern bars still originate at y = 0 internally, so log-scale
          # rendering emits benign transform warnings even though the PDF is
          # correct.
          suppressWarnings(save_plot())
        } else {
          save_plot()
        }
        cat("Saved", filepath, "\n")
      } else {
        grid.newpage()
        grid.draw(p)
      }
    }
  }
}

if (sys.nframe() == 0) {
  main()
}
