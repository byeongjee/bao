#!/usr/bin/env Rscript
# Plot intermittent-power results from results/intermittent/summary.csv.
#
# Normalized execution time of algorithm A on (benchmark, trace) =
# t_A / t_milp (BAO = 1), using execution_time_us (includes wait/outage time).
# (benchmark, trace) pairs where either run did not complete are dropped; the
# number of dropped traces is annotated under the bar.
#
# Outputs (log-scale y axis):
#   normalized_time_bar.pdf  geometric mean over traces per benchmark, plus the
#                            geometric mean over all benchmarks
#   normalized_time_box.pdf  distribution over traces per benchmark
#
# Usage:
#   Rscript scripts/plot_intermittent.R [--result-dir DIR] [--output-dir DIR]

suppressPackageStartupMessages({
  library(tidyverse)
  library(scales)
})

args <- commandArgs(trailingOnly = TRUE)
get_arg <- function(flag, default) {
  i <- match(flag, args)
  if (is.na(i)) default else args[i + 1]
}
result_dir <- get_arg("--result-dir", "results/intermittent")
output_dir <- get_arg("--output-dir", result_dir)

REF <- "milp"
ALGOS <- c(rockclimb = "ROCKCLIMB", schematic = "SCHEMATIC", schematicO3 = "SCHEMATIC-O3")
COLORS <- c(ROCKCLIMB = "#009E73", SCHEMATIC = "#D55E00", `SCHEMATIC-O3` = "#CC79A7")
BENCHMARK_LABELS <- c(activity_recognition = "ar")

theme_benchmark <- function() {
  theme_minimal(base_size = 15, base_family = "Helvetica") +
    theme(
      axis.title.x = element_blank(),
      axis.title.y = element_text(size = 15.5, margin = margin(r = 7)),
      axis.text.x = element_text(size = 14, angle = 15, hjust = 1, vjust = 1),
      axis.text.y = element_text(size = 12.5),
      legend.position = "top",
      legend.title = element_blank(),
      legend.text = element_text(size = 13),
      legend.margin = margin(0, 0, 0, 0),
      panel.grid.major.x = element_blank(),
      panel.grid.minor = element_blank(),
      panel.grid.major.y = element_line(color = "grey90", linewidth = 0.3),
      panel.border = element_rect(color = "grey80", fill = NA, linewidth = 0.4),
      plot.margin = margin(2, 2, 4, 2)
    )
}

summary <- read_csv(file.path(result_dir, "summary.csv"), show_col_types = FALSE)
bench_order <- unique(summary$benchmark)
bench_label <- function(b) {
  l <- BENCHMARK_LABELS[b]
  ifelse(is.na(l), b, l)
}

ok <- summary %>%
  filter(status == "ok") %>%
  select(benchmark, algorithm, trace, execution_time_us)
ref <- ok %>% filter(algorithm == REF) %>% select(benchmark, trace, t_ref = execution_time_us)

norm <- ok %>%
  filter(algorithm %in% names(ALGOS)) %>%
  inner_join(ref, by = c("benchmark", "trace")) %>%
  mutate(normalized = execution_time_us / t_ref,
         algo = factor(ALGOS[algorithm], levels = ALGOS),
         benchmark = factor(bench_label(benchmark), levels = bench_label(bench_order)))

n_traces <- n_distinct(summary$trace)
per_bench <- norm %>%
  group_by(benchmark, algo) %>%
  summarise(geomean = exp(mean(log(normalized))), dropped = n_traces - n(), .groups = "drop")
overall <- per_bench %>%
  group_by(algo) %>%
  summarise(geomean = exp(mean(log(geomean))), dropped = sum(dropped), .groups = "drop") %>%
  mutate(benchmark = "geomean")
bars <- bind_rows(per_bench, overall) %>%
  mutate(benchmark = factor(benchmark, levels = c(levels(norm$benchmark), "geomean")))

cat("geomean normalized execution time (BAO = 1), dropped traces:\n")
print(overall %>% select(algo, geomean, dropped), n = Inf)

y_scale <- scale_y_log10(breaks = c(1, 2, 5, 10, 20, 50, 100, 200, 500),
                         labels = label_number(accuracy = 1, suffix = "x"))

p_bar <- ggplot(bars, aes(benchmark, geomean, fill = algo)) +
  geom_col(position = position_dodge(0.8), width = 0.75) +
  geom_text(data = filter(bars, dropped > 0), aes(label = paste0("-", dropped), y = 1),
            position = position_dodge(0.8), vjust = 1.4, size = 3.2) +
  geom_hline(yintercept = 1, linetype = "dashed", linewidth = 0.4) +
  geom_vline(xintercept = length(levels(bars$benchmark)) - 0.5, color = "grey60", linewidth = 0.4) +
  scale_fill_manual(values = COLORS) +
  y_scale +
  labs(y = "Norm. execution time (BAO = 1)") +
  theme_benchmark()
ggsave(file.path(output_dir, "normalized_time_bar.pdf"), p_bar, width = 11, height = 3.95)

# Last group: distribution of per-benchmark geomeans; diamonds mark the geomean.
box_data <- bind_rows(
  norm %>% select(benchmark, algo, normalized),
  per_bench %>% transmute(benchmark = "geomean", algo, normalized = geomean)
) %>% mutate(benchmark = factor(benchmark, levels = levels(bars$benchmark)))
p_box <- ggplot(box_data, aes(benchmark, normalized, fill = algo)) +
  geom_boxplot(position = position_dodge(0.8), width = 0.7, outlier.size = 0.8, linewidth = 0.35) +
  geom_point(data = bars, aes(benchmark, geomean, group = algo), shape = 23, size = 1.8,
             fill = "white", position = position_dodge(0.8)) +
  geom_hline(yintercept = 1, linetype = "dashed", linewidth = 0.4) +
  geom_vline(xintercept = length(levels(bars$benchmark)) - 0.5, color = "grey60", linewidth = 0.4) +
  scale_fill_manual(values = COLORS) +
  y_scale +
  labs(y = "Norm. execution time (BAO = 1)") +
  theme_benchmark()
ggsave(file.path(output_dir, "normalized_time_box.pdf"), p_box, width = 11, height = 3.95)
