# Shared by plot_results.R and plot_intermittent.R: package setup, plot
# config parsing, ggpattern fill styles, and the two-line y-axis title.
# Callers define script_dir before sourcing this file.

local_r_lib <- file.path(dirname(script_dir), ".Rlib")
if (dir.exists(local_r_lib)) {
  .libPaths(c(local_r_lib, .libPaths()))
}

suppressPackageStartupMessages({
  library(tidyverse)
  library(scales)
  library(grid)
  library(gtable)
})

if (!requireNamespace("jsonlite", quietly = TRUE)) {
  stop("jsonlite is required to read the plot config. ",
       "Install it with install.packages(\"jsonlite\").")
}


# -- Series definitions (from --config) ---------------------------------------
#
# No CSV name is hardcoded here. The config lists two kinds of series:
#
#   algorithms  per-capacitor results, one bar group per capacitor plot. Files
#               default to "<algo>.csv" / "<algo>_debug.csv" (with the
#               "-swbor-no-debug" / "-swbor" spellings as fallbacks); override
#               with explicit "csv" / "debug_csv".
#   baselines   capacitor-independent references (e.g. uninstrumented builds),
#               replicated across benchmarks. "csv" is required. Optional
#               "metrics" restricts the baseline to specific metric keys, and
#               "normalize_ref" marks the series normalization divides by.
#
# Entry order defines bar and legend order; algorithms come before baselines.

DEFAULT_CONFIG_PATH <- file.path(script_dir, "plot_config.json")

config_field <- function(entry, name, default) {
  value <- entry[[name]]
  if (is.null(value)) default else value
}

parse_series_style <- function(entry, kind) {
  if (is.null(entry$algo) || is.null(entry$label)) {
    stop("Each ", kind, " entry needs an \"algo\" id and a \"label\".")
  }
  tibble(
    algo = as.character(entry$algo),
    label = as.character(entry$label),
    color = as.character(config_field(entry, "color", "#595959")),
    pattern = as.character(config_field(entry, "pattern", "none")),
    pattern_angle = as.numeric(config_field(entry, "pattern_angle", 0))
  )
}

load_plot_config <- function(path) {
  if (!file.exists(path)) {
    stop("Plot config not found: ", path)
  }
  raw <- jsonlite::fromJSON(path, simplifyVector = FALSE)

  if (length(raw$algorithms) == 0) {
    stop("Plot config has no \"algorithms\" entries: ", path)
  }

  algorithms <- map_dfr(raw$algorithms, function(entry) {
    parse_series_style(entry, "algorithm") %>%
      mutate(
        csv = as.character(config_field(entry, "csv", NA_character_)),
        debug_csv = as.character(config_field(entry, "debug_csv", NA_character_))
      )
  })

  baselines <- map_dfr(raw$baselines, function(entry) {
    if (is.null(entry$csv)) {
      stop("Baseline \"", entry$algo, "\" needs a \"csv\" filename.")
    }
    parse_series_style(entry, "baseline") %>%
      mutate(
        csv = as.character(entry$csv),
        metrics = list(as.character(config_field(entry, "metrics", character()))),
        normalize_ref = isTRUE(config_field(entry, "normalize_ref", FALSE))
      )
  })

  style <- bind_rows(
    select(algorithms, algo, label, color, pattern, pattern_angle),
    if (nrow(baselines) > 0) {
      select(baselines, algo, label, color, pattern, pattern_angle)
    }
  )
  duplicated_ids <- unique(style$algo[duplicated(style$algo)])
  if (length(duplicated_ids) > 0) {
    stop("Duplicate series ids in ", path, ": ",
         paste(duplicated_ids, collapse = ", "))
  }

  norm_ref <- if (nrow(baselines) > 0) baselines$algo[baselines$normalize_ref] else character()
  if (length(norm_ref) > 1) {
    stop("More than one baseline is marked \"normalize_ref\" in ", path, ": ",
         paste(norm_ref, collapse = ", "))
  }

  list(
    algorithms = algorithms,
    baselines = baselines,
    style = style,
    norm_ref = if (length(norm_ref) == 1) norm_ref else NA_character_
  )
}

# -- Fill patterns -------------------------------------------------------------

PATTERN_FILL_COLOUR <- "white"
PATTERN_COLOUR <- "grey10"
PATTERN_DENSITY <- 0.3
PATTERN_SPACING <- 0.055
PATTERN_UNITS <- "in"
PATTERN_ALPHA <- 1.0
PATTERN_SIZE <- 0.3
PATTERN_LEGEND_SCALE <- 1.0
PATTERN_LEGEND_DENSITY <- 0.42
PATTERN_LEGEND_SPACING <- 0.055
PATTERN_LEGEND_ALPHA <- 1.0
PATTERN_LEGEND_SIZE <- 0.42

# Compact paper figure dimensions. Preserve readable text and reclaim vertical
# space from the legend before shrinking the data panel.
PLOT_HEIGHT_IN <- 2.6
LEGEND_KEY_HEIGHT_CM <- 0.55
AXIS_TITLE_SIZE <- 13
AXIS_SUBTITLE_SIZE <- 9.5

# -- Axis title ---------------------------------------------------------------

# Y-axis title with a smaller second line. Plotmath's atop() pads the two
# lines far more than the text needs, so the title grob is built directly and
# swapped into the rendered gtable.
stack_ylab <- function(p, main, sub) {
  main_g <- textGrob(main, rot = 90,
                     gp = gpar(fontsize = AXIS_TITLE_SIZE,
                               fontfamily = "Helvetica"))
  sub_g <- textGrob(sub, rot = 90,
                    gp = gpar(fontsize = AXIS_SUBTITLE_SIZE,
                              fontfamily = "Helvetica"))
  # Column widths are line heights rather than grobWidth(), which ignores
  # descenders.
  title <- gtable(
    widths = unit(c(AXIS_TITLE_SIZE * 1.2,
                    AXIS_SUBTITLE_SIZE * 1.2, 7), "pt"),
    heights = unit(1, "null")
  )
  title <- gtable_add_grob(title, list(main_g, sub_g), t = 1, l = c(1, 2))
  gt <- ggplotGrob(p)
  idx <- which(gt$layout$name == "ylab-l")
  gt$grobs[[idx]] <- title
  gt$widths[gt$layout$l[idx]] <- sum(title$widths)
  gt
}
