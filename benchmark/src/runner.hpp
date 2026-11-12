#pragma once
#include "config.hpp"

#include <filesystem>

class Runner {
public:
	explicit Runner(plume::bench::BenchmarkConfig cfg, std::filesystem::path path) : cfg_(std::move(cfg)), cfg_path_(std::move(path)) {}

	const plume::bench::BenchmarkConfig &Config() const { return cfg_; }

	void Run();
private:
	plume::bench::BenchmarkConfig cfg_;
	std::filesystem::path cfg_path_;
};