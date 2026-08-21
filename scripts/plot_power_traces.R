#!/usr/bin/env Rscript

# The replay-ready traces are 80 back-to-back repetitions of one compressed
# walk (see benchmarks/traces/README.md); plot a single period of each.
repetitions <- 80

load_trace <- function(path) {
  values <- read.csv(path, colClasses = c("numeric", "numeric"))
  period <- nrow(values) %/% repetitions
  values <- values[seq_len(period), ]
  list(
    id = as.integer(tools::file_path_sans_ext(basename(path))),
    time = values$time_s,
    voltage = values$voltage_v
  )
}

main <- function() {
  args <- commandArgs(trailingOnly = TRUE)
  if (length(args) < 1 || length(args) > 2) {
    stop("Usage: Rscript plot_power_traces.R TRACE_DIR [OUTPUT.pdf]")
  }
  trace_dir <- args[1]
  output_path <- if (length(args) == 2) {
    args[2]
  } else {
    file.path(trace_dir, "traces_grid.pdf")
  }

  trace_files <- list.files(
    trace_dir,
    pattern = "^[0-9]+[.]csv$",
    full.names = TRUE
  )
  trace_files <- trace_files[
    order(as.integer(tools::file_path_sans_ext(basename(trace_files))))
  ]
  if (length(trace_files) == 0) {
    stop("No numeric trace files found in ", trace_dir)
  }

  traces <- lapply(trace_files, load_trace)
  voltage_max <- max(vapply(
    traces,
    function(trace) max(trace$voltage),
    numeric(1)
  ))

  # Headroom above the highest sample so the panel labels never overlap.
  y_top <- voltage_max * 1.15

  rows <- 2
  cols <- 5
  pdf(output_path, width = 3.35, height = 1.9, pointsize = 8)
  on.exit(if (dev.cur() > 1) dev.off(), add = TRUE)
  par(
    mfrow = c(rows, cols),
    mar = c(1.2, 0.35, 0.35, 0.2),
    oma = c(1.4, 2.4, 0.2, 0.2),
    mgp = c(1.5, 0.25, 0),
    tcl = -0.2
  )

  for (i in seq_along(traces)) {
    trace <- traces[[i]]
    left_column <- i %% cols == 1
    time_max <- max(trace$time)

    plot(
      NA,
      xlim = c(0, time_max),
      ylim = c(0, y_top),
      axes = FALSE,
      xlab = "",
      ylab = ""
    )
    lines(trace$time, trace$voltage, col = "#26619c", lwd = 0.5)
    box(lwd = 0.6)
    axis(1, at = pretty(c(0, time_max), n = 2), lwd = 0.6, cex.axis = 0.8)
    axis(2, at = c(0, 1.5, 3), labels = left_column, lwd = 0.6, cex.axis = 0.8)
    text(
      x = time_max * 0.96,
      y = y_top * 0.97,
      labels = trace$id,
      adj = c(1, 1),
      cex = 0.85
    )
  }

  mtext("Time (s)", side = 1, outer = TRUE, line = 0.4, cex = 0.8)
  mtext("Voltage (V)", side = 2, outer = TRUE, line = 1.3, cex = 0.8)
  dev.off()

  message("Wrote ", normalizePath(output_path))
}

main()
