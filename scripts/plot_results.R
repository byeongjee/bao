#!/usr/bin/env Rscript
# Visualize benchmark results from result/ directory using ggplot2.
#
# Reads *_debug.csv for runtime region boundary metrics and
# *.csv (non-debug) for execution/profiling/compilation times.
#
# Usage:
#   Rscript scripts/plot_results.R [OPTIONS]
#
# Options:
#   --result-dir DIR    Result directory (default: results/)
#   --normalize         Normalize values (w.r.t. uninstrumented if available, else milp)
#   --output-dir DIR    Save plots to directory instead of showing
#   --benchmarks B,...  Comma-separated benchmark filter (default: all)
#   --metrics M,...     Comma-separated metric filter (default: all)
#   --log-scale         Force log scale for execution_time and runtime_region_boundary_calls

script_args <- commandArgs(trailingOnly = FALSE)
script_file_arg <- script_args[grepl("^--file=", script_args)]
script_path <- if (length(script_file_arg) > 0) {
  sub("^--file=", "", script_file_arg[1])
} else {
  NA_character_
}
local_r_lib <- if (!is.na(script_path)) {
  file.path(dirname(dirname(normalizePath(script_path))), ".Rlib")
} else {
  NA_character_
}
if (!is.na(local_r_lib) && dir.exists(local_r_lib)) {
  .libPaths(c(local_r_lib, .libPaths()))
}

suppressPackageStartupMessages({
  library(tidyverse)
  library(scales)
  library(grid)
})

HAS_GGPATTERN <- requireNamespace("ggpattern", quietly = TRUE)

# -- Algorithm definitions ----------------------------------------------------

ALGORITHMS <- c("milp", "schematic", "rockclimb", "schematicO3")

ALG_STYLE <- tribble(
  ~algo,             ~label,            ~color,     ~pattern,      ~pattern_angle,
  "milp",            "MILP",            "#0072B2",  "stripe",      45,
  "schematic",       "SCHEMATIC",       "#D55E00",  "stripe",     -45,
  "rockclimb",       "RockClimb",       "#009E73",  "crosshatch",   0,
  "schematicO3",     "SCHEMATIC-O3",    "#CC79A7",  "stripe",       0,
  "uninstrumented",  "Uninstrumented",  "#595959",  "none",         0,
)

# -- Metric definitions -------------------------------------------------------

METRICS <- list(
  region_boundaries = list(
    source = "debug", column = "region_boundaries",
    include_uninstrumented = FALSE,
    ylabel = "# Region Boundaries (static)",
    title = "Region Boundaries"
  ),
  runtime_region_boundary_calls = list(
    source = "debug", column = "runtime_region_boundary_calls",
    include_uninstrumented = FALSE,
    ylabel = "# Runtime Region Boundary Calls",
    title = "Runtime Region Boundary Calls"
  ),
  execution_time = list(
    source = "no-debug", column = "execution_time_us",
    include_uninstrumented = TRUE,
    ylabel = expression("Execution Time (" * mu * "s)"),
    title = "Execution Time"
  ),
  profiling_time = list(
    source = "no-debug", column = "profiling_time_ms",
    include_uninstrumented = FALSE,
    ylabel = "Profiling Time (ms)",
    title = "Profiling Time"
  ),
  compilation_time = list(
    source = "no-debug", column = "compilation_time_ms",
    include_uninstrumented = TRUE,
    ylabel = "Compilation Time (ms)",
    title = "Compilation Time"
  )
)

# -- Theme --------------------------------------------------------------------

theme_benchmark <- function() {
  theme_minimal(base_size = 10.5, base_family = "Helvetica") +
    theme(
      plot.title = element_text(face = "bold", size = 12, hjust = 0.5),
      plot.subtitle = element_text(color = "grey35", size = 9, hjust = 0.5),
      plot.caption = element_text(color = "grey35", size = 7.5, hjust = 0),
      axis.title.x = element_text(margin = margin(t = 6)),
      axis.title.y = element_text(margin = margin(r = 6)),
      axis.text.x = element_text(size = 8.5, angle = 20, hjust = 1, vjust = 1),
      axis.text.y = element_text(size = 8.5),
      legend.position = "top",
      legend.title = element_blank(),
      legend.text = element_text(size = 9),
      legend.key.size = unit(0.42, "cm"),
      legend.box.spacing = unit(0.05, "cm"),
      legend.margin = margin(0, 0, 2, 0),
      panel.grid.major.x = element_blank(),
      panel.grid.minor = element_blank(),
      panel.grid.major.y = element_line(color = "grey90", linewidth = 0.3),
      panel.border = element_rect(color = "grey80", fill = NA, linewidth = 0.4),
      plot.margin = margin(6, 8, 8, 6)
    )
}

# -- Data loading -------------------------------------------------------------

parse_benchmark_cap <- function(benchmark) {
  # Split "name-cap" into (name, cap). E.g. "aes-5uF" -> ("aes", "5uF")
  parts <- str_match(benchmark, "^(.+)-(\\d+uF)$")
  if (is.na(parts[1, 1])) {
    return(tibble(name = benchmark, cap = NA_character_))
  }
  tibble(name = parts[1, 2], cap = parts[1, 3])
}

resolve_csv_path <- function(result_dir, algo, source) {
  candidates <- if (source == "debug") {
    c(file.path(result_dir, paste0(algo, "_debug.csv")),
      file.path(result_dir, paste0(algo, "-swbor.csv")))
  } else {
    c(file.path(result_dir, paste0(algo, ".csv")),
      file.path(result_dir, paste0(algo, "-swbor-no-debug.csv")))
  }
  found <- candidates[file.exists(candidates)]
  if (length(found) > 0) found[1] else NA_character_
}

load_algorithm_data <- function(result_dir, algo, source, column) {
  path <- resolve_csv_path(result_dir, algo, source)
  if (is.na(path)) return(tibble())

  df <- read_csv(path, show_col_types = FALSE) %>%
    filter(!is.na(benchmark), benchmark != "")

  if (!column %in% names(df)) return(tibble())

  df %>%
    mutate(parsed = map(benchmark, parse_benchmark_cap)) %>%
    unnest(parsed) %>%
    filter(!is.na(cap)) %>%
    transmute(
      benchmark = name,
      cap = cap,
      algo = algo,
      value = as.numeric(.data[[column]])
    ) %>%
    filter(!is.na(value))
}

load_uninstrumented_data <- function(result_dir, column) {
  path <- file.path(result_dir, "uninstrumented.csv")
  if (!file.exists(path)) return(tibble())

  df <- read_csv(path, show_col_types = FALSE) %>%
    filter(!is.na(benchmark), benchmark != "")

  if (!column %in% names(df)) return(tibble())

  df %>%
    transmute(
      benchmark = benchmark,
      cap = NA_character_,
      algo = "uninstrumented",
      value = as.numeric(.data[[column]])
    ) %>%
    filter(!is.na(value))
}

discover_capacitors <- function(result_dir) {
  caps <- character()
  for (algo in ALGORITHMS) {
    for (source in c("debug", "no-debug")) {
      path <- resolve_csv_path(result_dir, algo, source)
      if (is.na(path)) next
      df <- read_csv(path, show_col_types = FALSE)
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

LOG_SCALE_METRICS <- c("execution_time", "runtime_region_boundary_calls")
OUTLIER_RATIO_THRESHOLD <- 3.0
OUTLIER_HEADROOM <- 1.15

format_metric_value <- function(value, normalize) {
  if (normalize) {
    return(sprintf("%.2f", value))
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
                                algo_data, uninst_data, benchmarks, normalize,
                                log_scale = FALSE) {
  include_uninst <- metric_info$include_uninstrumented && nrow(uninst_data) > 0

  # Filter to this capacitor
  df <- algo_data %>% filter(cap == !!cap)

  if (include_uninst) {
    # Replicate uninstrumented for each benchmark (cap-independent)
    uninst_for_cap <- uninst_data %>%
      filter(benchmark %in% benchmarks) %>%
      mutate(cap = !!cap)
    df <- bind_rows(df, uninst_for_cap)
  }

  df <- df %>% filter(benchmark %in% benchmarks)
  if (nrow(df) == 0) return(NULL)

  force_log <- log_scale && metric_key %in% LOG_SCALE_METRICS

  # Determine normalization (geomean is computed after this step)
  norm_algo <- NULL
  if (normalize) {
    norm_algo <- if (include_uninst) "uninstrumented" else "milp"
    base <- df %>%
      filter(algo == norm_algo) %>%
      select(benchmark, base_value = value)
    df <- df %>%
      left_join(base, by = "benchmark") %>%
      mutate(value = if_else(!is.na(base_value) & base_value != 0,
                             value / base_value, NA_real_)) %>%
      select(-base_value)
  }

  # Compute geometric mean across benchmarks for each algorithm
  geomean_df <- df %>%
    filter(!is.na(value), value > 0) %>%
    group_by(algo) %>%
    summarise(value = exp(mean(log(value))), .groups = "drop") %>%
    mutate(benchmark = "geomean", cap = !!cap)
  df <- bind_rows(df, geomean_df)
  benchmarks_with_geomean <- c(benchmarks, "geomean")

  # Set factor levels for ordering
  active_algos <- intersect(
    c(ALGORITHMS, if (include_uninst) "uninstrumented"),
    unique(df$algo)
  )
  label_map <- setNames(
    ALG_STYLE$label[match(active_algos, ALG_STYLE$algo)],
    active_algos
  )
  color_map <- setNames(
    ALG_STYLE$color[match(active_algos, ALG_STYLE$algo)],
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

  # Build subtitle
  if (normalize && !is.null(norm_algo)) {
    norm_label <- ALG_STYLE$label[ALG_STYLE$algo == norm_algo]
    subtitle <- paste0(cap, " - normalized to ", norm_label)
    y_label <- paste0("Normalized to ", norm_label)
  } else {
    subtitle <- cap
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

  bar_position <- position_dodge2(width = 0.84, preserve = "single",
                                  padding = 0.08)
  color_map <- setNames(
    ALG_STYLE$color[match(active_algos, ALG_STYLE$algo)],
    label_map[active_algos]
  )

  p <- ggplot(plot_df, aes(x = benchmark, y = display_value,
                           fill = algo_label, group = algo_label))
  if (HAS_GGPATTERN) {
    pattern_map <- setNames(
      ALG_STYLE$pattern[match(active_algos, ALG_STYLE$algo)],
      label_map[active_algos]
    )
    pattern_angle_map <- setNames(
      ALG_STYLE$pattern_angle[match(active_algos, ALG_STYLE$algo)],
      label_map[active_algos]
    )
    p <- p + ggpattern::geom_col_pattern(
      aes(pattern = algo_label, pattern_angle = algo_label),
      position = bar_position,
      width = 0.72,
      color = "grey20",
      linewidth = 0.35,
      pattern_fill = "white",
      pattern_colour = "grey10",
      pattern_density = 0.18,
      pattern_spacing = 0.065,
      pattern_alpha = 0.7,
      pattern_size = 0.18,
      pattern_key_scale_factor = 0.6
    ) +
      ggpattern::scale_pattern_manual(values = pattern_map, drop = FALSE) +
      ggpattern::scale_pattern_angle_manual(values = pattern_angle_map,
                                            drop = FALSE)
  } else {
    p <- p + geom_col(
      position = bar_position,
      width = 0.72,
      color = "grey20",
      linewidth = 0.35
    )
  }

  p <- p +
    scale_fill_manual(values = color_map, drop = FALSE) +
    labs(
      title = metric_info$title,
      subtitle = subtitle,
      x = NULL,
      y = y_label,
      caption = if (length(plot_notes) > 0) paste(plot_notes, collapse = "\n") else NULL
    ) +
    guides(fill = guide_legend(nrow = 1)) +
    theme_benchmark()

  # Add value labels on bars
  label_df <- plot_df %>%
    filter(!is.na(display_value), display_value > 0, !clipped)
  if (nrow(label_df) > 0) {
    p <- p + geom_text(
      data = label_df,
      aes(label = vapply(value, format_metric_value,
                         FUN.VALUE = character(1), normalize = normalize)),
      position = bar_position,
      vjust = -0.35, size = 2.2, color = "grey25"
    )
  }

  if (nrow(clipped_df) > 0) {
    p <- p +
      geom_segment(
        data = clipped_df,
        aes(x = benchmark, xend = benchmark,
            y = display_value / OUTLIER_HEADROOM, yend = display_value,
            group = algo_label),
        inherit.aes = FALSE,
        position = bar_position,
        linewidth = 0.35,
        color = "grey15",
        arrow = arrow(type = "closed", length = unit(0.07, "in"))
      ) +
      geom_text(
        data = clipped_df,
        aes(label = paste0(
          vapply(value, format_metric_value,
                 FUN.VALUE = character(1), normalize = normalize),
          " ^"
        )),
        position = bar_position,
        vjust = -0.2,
        size = 2.25,
        color = "grey15",
        fontface = "bold"
      )
  }

  # Normalization reference line
  if (normalize && !is.null(norm_algo)) {
    p <- p + geom_hline(yintercept = 1.0, linetype = "dashed",
                        color = "grey50", linewidth = 0.5)
  }

  top_display_value <- max(plot_df$display_value, na.rm = TRUE)
  top_padding <- if (nrow(clipped_df) > 0) 1.08 else 1.04

  if (force_log) {
    # Log-scaled bar chart: use coord_trans so bars render properly,
    # clip the y-axis just below the minimum value
    y_lo <- min(pos_vals, na.rm = TRUE) * 0.88
    y_hi <- top_display_value * top_padding
    p <- p +
      scale_y_continuous(
        labels = axis_labeler,
        breaks = compute_transformed_breaks(pos_vals, include_zero = FALSE)
      ) +
      coord_transform(y = "log10", ylim = c(y_lo, y_hi), clip = "off") +
      labs(subtitle = paste0(subtitle, " [log scale]"))
  } else if (use_symlog) {
    p <- p + scale_y_continuous(
      trans = pseudo_log_trans(base = 10),
      labels = axis_labeler,
      breaks = compute_transformed_breaks(pos_vals, include_zero = TRUE),
      expand = expansion(mult = c(0, 0.03))
    ) +
      coord_cartesian(ylim = c(0, top_display_value * top_padding),
                      clip = "off") +
      labs(subtitle = paste0(subtitle, " [pseudo-log scale]"))
  } else {
    p <- p + scale_y_continuous(
      expand = expansion(mult = c(0, 0.03)),
      labels = axis_labeler,
      n.breaks = 5
    ) +
      coord_cartesian(ylim = c(0, top_display_value * top_padding),
                      clip = "off")
  }

  p
}

# -- Main ---------------------------------------------------------------------

main <- function() {
  args <- commandArgs(trailingOnly = TRUE)

  # Parse arguments
  result_dir <- "results"
  normalize <- FALSE
  log_scale <- FALSE
  output_dir <- NULL
  filter_benchmarks <- NULL
  filter_metrics <- NULL

  i <- 1
  while (i <= length(args)) {
    if (args[i] == "--result-dir" && i < length(args)) {
      result_dir <- args[i + 1]; i <- i + 2
    } else if (args[i] == "--normalize") {
      normalize <- TRUE; i <- i + 1
    } else if (args[i] == "--log-scale") {
      log_scale <- TRUE; i <- i + 1
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

  if (!is.null(output_dir)) {
    dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
  }

  metrics_to_plot <- if (!is.null(filter_metrics)) {
    filter_metrics[filter_metrics %in% names(METRICS)]
  } else {
    names(METRICS)
  }

  # Discover benchmarks
  all_benchmarks <- character()
  for (algo in ALGORITHMS) {
    for (source in c("debug", "no-debug")) {
      path <- resolve_csv_path(result_dir, algo, source)
      if (is.na(path)) next
      df <- read_csv(path, show_col_types = FALSE)
      bm <- df$benchmark[!is.na(df$benchmark) & df$benchmark != ""]
      parsed <- map_dfr(bm, parse_benchmark_cap)
      all_benchmarks <- c(all_benchmarks, parsed$name[!is.na(parsed$name)])
    }
  }
  uninst_path <- file.path(result_dir, "uninstrumented.csv")
  if (file.exists(uninst_path)) {
    df <- read_csv(uninst_path, show_col_types = FALSE)
    all_benchmarks <- c(all_benchmarks, df$benchmark[!is.na(df$benchmark)])
  }
  all_benchmarks <- unique(all_benchmarks)

  if (!is.null(filter_benchmarks)) {
    benchmarks <- filter_benchmarks[filter_benchmarks %in% all_benchmarks]
  } else {
    real <- setdiff(all_benchmarks, "test")
    benchmarks <- sort(if (length(real) > 0) real else all_benchmarks)
  }

  if (length(benchmarks) == 0) stop("No benchmarks found.")

  capacitors <- discover_capacitors(result_dir)

  cat("Benchmarks:", paste(benchmarks, collapse = ", "), "\n")
  cat("Metrics:", paste(metrics_to_plot, collapse = ", "), "\n")
  cat("Capacitors:", paste(capacitors, collapse = ", "), "\n")

  for (metric_key in metrics_to_plot) {
    metric_info <- METRICS[[metric_key]]
    source <- metric_info$source
    column <- metric_info$column

    algo_data <- map_dfr(ALGORITHMS, function(algo) {
      load_algorithm_data(result_dir, algo, source, column)
    })

    uninst_data <- if (metric_info$include_uninstrumented) {
      load_uninstrumented_data(result_dir, column)
    } else {
      tibble()
    }

    for (cap in capacitors) {
      p <- plot_metric_for_cap(
        cap, metric_key, metric_info,
        algo_data, uninst_data, benchmarks, normalize, log_scale
      )
      if (is.null(p)) next

      if (!is.null(output_dir)) {
        norm_suffix <- if (normalize) "_normalized" else ""
        log_suffix <- if (log_scale && metric_key %in% LOG_SCALE_METRICS) "_log" else ""
        filename <- paste0(metric_key, "_", cap, norm_suffix, log_suffix, ".pdf")
        filepath <- file.path(output_dir, filename)

        n_bm <- length(benchmarks) + 1  # +1 for geomean
        w <- max(6.2, n_bm * 0.95 + 1.5)

        save_plot <- function() {
          ggsave(filepath, p, width = w, height = 4.35, device = "pdf")
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
        print(p)
      }
    }
  }
}

main()
