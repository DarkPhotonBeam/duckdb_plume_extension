#include "config.hpp"
#include "log.hpp"
#include "runner.hpp"

#include <cxxopts.hpp>
#include <iostream>

int main(int argc, char **argv) {
	// local alias: a global one would clash with ::log from <cmath>
	namespace log = plume_ext::log;
	namespace fs = std::filesystem;

	cxxopts::Options opts("plume_ext_bench", "Benchmarks the plume extension");

	opts.add_options()
		("d,cfg-dir", "Directory where the provided config files reside", cxxopts::value<std::string>())
		("v,verbose", "Print debug output")
		("h,help", "Show help")
		("cfg_files", "Config files", cxxopts::value<std::vector<std::string>>())
		("u,dandelion-url", "Overrides the dandelion url set in config files", cxxopts::value<std::string>())
	;

	opts.parse_positional({"cfg_files"});
	opts.positional_help("<cfg_file> [<cfg_file>...]");

	cxxopts::ParseResult result;
	try {
		result = opts.parse(argc, argv);
	} catch (const cxxopts::exceptions::exception &e) {
		log::Error(e.what());
		std::cerr << opts.help() << '\n';
		return 2;
	}

	if (result.count("help")) {
		std::cout << opts.help() << '\n';
		return 0;
	}
	if (result.count("verbose")) {
		log::SetLevel(log::Level::Debug);
	}
	if (!result.count("cfg_files")) {
		log::Error("no input files");
		std::cerr << opts.help() << '\n';
		return 2;
	}

	auto dandelion_url = result["dandelion-url"].as_optional<std::string>();

	if (dandelion_url.has_value()) {
		log::Info("=== dandelion url override: ", dandelion_url.value(), " ===");
	}

	fs::path cfg_dir = "";
	if (result.count("cfg-dir")) {
		log::Debug("cfg-dir = ", result["cfg-dir"].as<std::string>());
		cfg_dir = result["cfg-dir"].as<std::string>();
	}

	std::vector<Runner> runners{};

	int rc = 0;
	const auto &cfg_files = result["cfg_files"].as<std::vector<std::string>>();
	for (const auto &path_str : cfg_files) {
		auto path = cfg_dir / path_str;

		log::Info("=== loading config ", path.filename(), " ===");
		auto cfg = plume::bench::BenchmarkConfig::FromJsonFile(path);
		if (cfg.is_error()) {
			log::Error(cfg.error().message());
			rc = 1;
			continue;
		}
		runners.emplace_back(std::move(cfg).unwrap(), path);
		log::Debug("loaded config ", path.filename());
	}

	for (auto &runner : runners) {
		runner.Run();
	}

	return rc;
}
