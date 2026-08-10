#!/usr/bin/env Rscript

timestamp_to_seconds <- function(timestamp) {
  milliseconds <- timestamp %% 1000
  clock <- timestamp %/% 1000
  seconds <- clock %% 100
  minutes <- (clock %/% 100) %% 100
  hours <- clock %/% 10000

  hours * 3600 + minutes * 60 + seconds + milliseconds / 1000
}

load_trace <- function(path) {
  values <- read.table(
    path,
    header = FALSE,
    col.names = c("timestamp", "voltage"),
    colClasses = c("numeric", "numeric")
  )
  recorded_elapsed <- timestamp_to_seconds(values$timestamp)
  recorded_elapsed <- recorded_elapsed - recorded_elapsed[1]
  expected_elapsed <- (seq_len(nrow(values)) - 1) / 1000

  list(
    id = as.integer(tools::file_path_sans_ext(basename(path))),
    elapsed = expected_elapsed,
    voltage = values$voltage,
    timestamp_glitch = abs(recorded_elapsed - expected_elapsed) > 0.0005
  )
}

main <- function() {
  args <- commandArgs(trailingOnly = TRUE)
  if (length(args) < 1 || length(args) > 2) {
    stop("Usage: Rscript plot_power_traces.R TRACE_DIR [OUTPUT.png]")
  }
  trace_dir <- args[1]
  output_path <- if (length(args) == 2) {
    args[2]
  } else {
    file.path(trace_dir, "traces_grid.png")
  }

  trace_files <- list.files(
    trace_dir,
    pattern = "^[0-9]+[.]txt$",
    full.names = TRUE
  )
  trace_files <- trace_files[
    order(as.integer(tools::file_path_sans_ext(basename(trace_files))))
  ]
  if (length(trace_files) == 0) {
    stop("No numeric trace files found in ", trace_dir)
  }

  traces <- lapply(trace_files, load_trace)
  voltage_range <- range(
    unlist(lapply(traces, function(trace) trace$voltage)),
    finite = TRUE
  )

  png(output_path, width = 2500, height = 1200, res = 180)
  on.exit(if (dev.cur() > 1) dev.off(), add = TRUE)
  par(
    mfrow = c(2, 5),
    mar = c(3.2, 3.4, 2.2, 0.7),
    oma = c(2.5, 2.8, 1.2, 0.5),
    mgp = c(2, 0.7, 0),
    tcl = -0.25
  )

  for (trace in traces) {
    plot(
      trace$elapsed,
      trace$voltage,
      type = "n",
      xlab = "",
      ylab = "",
      ylim = voltage_range,
      main = paste("Trace", trace$id)
    )
    grid(col = "grey90", lty = 1)
    lines(trace$elapsed, trace$voltage, col = "#26619c", lwd = 0.7)
    if (any(trace$timestamp_glitch)) {
      points(
        trace$elapsed[trace$timestamp_glitch],
        trace$voltage[trace$timestamp_glitch],
        col = "#c62828",
        pch = 4,
        lwd = 1.5
      )
    }
  }

  mtext("Elapsed time (s)", side = 1, outer = TRUE, line = 1)
  mtext("Voltage (V)", side = 2, outer = TRUE, line = 1.2)
  mtext(
    "Red × = timestamp anomaly",
    side = 3,
    outer = TRUE,
    line = 0.1,
    adj = 1,
    col = "#c62828",
    cex = 0.75
  )
  dev.off()

  glitch_counts <- vapply(
    traces,
    function(trace) sum(trace$timestamp_glitch),
    integer(1)
  )
  glitches <- traces[glitch_counts > 0]
  message("Wrote ", normalizePath(output_path))
  if (length(glitches) > 0) {
    message(
      "Timestamp glitches marked in red: ",
      paste(
        vapply(
          glitches,
          function(trace) paste0("Trace ", trace$id),
          character(1)
        ),
        collapse = ", "
      )
    )
  }
}

main()
