#!/usr/bin/env Rscript

script_args <- commandArgs(trailingOnly = FALSE)
script_file_arg <- script_args[grepl("^--file=", script_args)]
script_path <- if (length(script_file_arg) > 0) {
  sub("^--file=", "", script_file_arg[1])
} else {
  NA_character_
}

if (is.na(script_path)) {
  stop("Unable to determine script path.")
}

source(file.path(dirname(normalizePath(script_path)), "plot_results.R"),
       local = FALSE)

ALGORITHMS <- c(
  "milp",
  "rockclimb",
  "rockclimb_64",
  "schematic",
  "schematicO3"
)

ALG_STYLE <- ALG_STYLE %>%
  add_row(
    algo = "rockclimb_64",
    label = "RockClimb 64",
    color = "#E69F00",
    pattern = "crosshatch",
    pattern_angle = 45
  )

REQUIRED_ALGO_FOR_BENCHMARKS <- "rockclimb_64"

main()
