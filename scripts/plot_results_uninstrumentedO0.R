#!/usr/bin/env Rscript
# Variant of plot_results.R that uses uninstrumentedO0.csv for execution-time
# baselines while keeping the rest of the behavior unchanged.

script_args <- commandArgs(trailingOnly = FALSE)
script_file_arg <- script_args[grepl("^--file=", script_args)]
script_path <- if (length(script_file_arg) > 0) {
  sub("^--file=", "", script_file_arg[1])
} else {
  NA_character_
}

plot_script_path <- if (!is.na(script_path)) {
  file.path(dirname(normalizePath(script_path)), "plot_results.R")
} else {
  file.path("scripts", "plot_results.R")
}

plot_env <- new.env(parent = globalenv())
sys.source(plot_script_path, envir = plot_env)

plot_env$ALGORITHMS <- c(plot_env$ALGORITHMS, "uninstrumentedO0")
plot_env$ALG_STYLE <- dplyr::bind_rows(
  plot_env$ALG_STYLE,
  tibble::tibble(
    algo = "uninstrumentedO0",
    label = "Uninstrumented-O0",
    color = "#9E9E9E",
    pattern = "stripe",
    pattern_angle = 135
  )
)

plot_env$load_uninstrumented_data <- function(result_dir, column) {
  load_single_uninstrumented <- function(filename, algo_name) {
    path <- file.path(result_dir, filename)
    if (!file.exists(path)) return(tibble::tibble())

    df <- plot_env$read_result_csv(path)
    if (nrow(df) == 0) return(tibble::tibble())

    df <- dplyr::filter(df, !is.na(benchmark), benchmark != "")

    if (!column %in% names(df)) return(tibble::tibble())

    tibble::tibble(
      benchmark = df$benchmark,
      cap = NA_character_,
      algo = algo_name,
      value = as.numeric(df[[column]])
    ) %>%
      dplyr::filter(!is.na(value))
  }

  base_uninstrumented <- load_single_uninstrumented(
    "uninstrumented.csv",
    "uninstrumented"
  )

  if (!identical(column, "execution_time_us")) {
    return(base_uninstrumented)
  }

  uninstrumented_o0 <- load_single_uninstrumented(
    "uninstrumentedO0.csv",
    "uninstrumentedO0"
  )

  dplyr::bind_rows(base_uninstrumented, uninstrumented_o0)
}

if (sys.nframe() == 0) {
  plot_env$main()
}
