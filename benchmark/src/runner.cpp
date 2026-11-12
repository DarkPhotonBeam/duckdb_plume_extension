#include "runner.hpp"
#include "log.hpp"
#include "plume_adapter_extension.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

#include <fstream>

bool IsIdent(const std::string &s) {
	if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')) {
		return false;
	}
	return std::all_of(s.begin(), s.end(),
					   [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	size_t e = s.find_last_not_of(" \t\r\n");
	return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string SourceExpr(const std::string &table, const plume::bench::DataSource &source) {
	auto it = source.num_files.find(table);
	const std::string base = source.path_prefix + "/" + table + "/" + table;
	if (it == source.num_files.end() || it->second <= 1) {
		if (it != source.num_files.end() && it->second == 1) {
			return "['" + base + ".1" + source.path_suffix + "']";
		}
		return "'" + base + source.path_suffix + "'";
	}
	std::string expr = "[";
	for (uint32_t i = 0; i < it->second; i++) {
		expr += (i ? ", " : "");
		expr += "'" + base + "." + std::to_string(i + 1) + source.path_suffix + "'";
	}
	expr += "]";
	return expr;
}

std::string DuckDBSourceExpr(const std::string &table, const plume::bench::DataSource &source) {
	return "read_parquet(" + SourceExpr(table, source) + ")";
}

using SourceExprFn = std::string (*)(const std::string &, const plume::bench::DataSource &);

std::string ResolvePlaceholders(const std::string &sql, const plume::bench::DataSource &source,
                                SourceExprFn source_expr = SourceExpr) {
	std::string out;
	out.reserve(sql.size());
	for (size_t i = 0; i < sql.size();) {
		if (sql[i] == '[') {
			size_t close = sql.find(']', i);
			if (close != std::string::npos) {
				std::string inner = Trim(sql.substr(i + 1, close - i - 1));
				if (IsIdent(inner)) {
					out += source_expr(inner, source);
					i = close + 1;
					continue;
				}
			}
		}
		out += sql[i++];
	}
	return out;
}

std::string ToPlumeExtQuery(const std::string &query) {
	return "SELECT * FROM plume_query($plume$" + query + "$plume$);";
}

struct ResolvedQuery {
	std::string query;
	std::string normal_query;
	std::string name;
	std::filesystem::path path;
};

void Runner::Run() {
	namespace log = plume_ext::log;
	namespace fs = std::filesystem;

	fs::path query_dir{cfg_.query_dir};

	std::vector<ResolvedQuery> resolved_queries{};

	for (auto &q : cfg_.queries) {
		fs::path query_path = query_dir / q;
		std::ifstream sql_file(query_path);
		if (!sql_file.is_open()) {
			throw;
		}
		std::string query((std::istreambuf_iterator<char>(sql_file)), std::istreambuf_iterator<char>());

		ResolvedQuery rq{
		    .query = ToPlumeExtQuery(ResolvePlaceholders(query, cfg_.source)),
			.normal_query = ResolvePlaceholders(query, cfg_.source, DuckDBSourceExpr),
			.name = q,
			.path = query_path
		};
		resolved_queries.push_back(rq);

		log::Debug("resolved query ", rq.name);
	}

	log::Debug("running with config ", cfg_path_.filename());

	duckdb::DuckDB db;
	db.LoadStaticExtension<duckdb::PlumeAdapterExtension>();
	duckdb::Connection con{db};

	// Connection::Query doesn't throw on failure; it returns a result with HasError() set.
	auto exec = [&con](const std::string &sql) -> bool {
		auto res = con.Query(sql);
		if (res->HasError()) {
			log::Error("query failed: ", sql, "\n  ", res->GetError());
			return false;
		}
		return true;
	};

	const auto &prefix = cfg_.source.path_prefix;
	if (prefix.rfind("http://", 0) == 0 || prefix.rfind("https://", 0) == 0 || prefix.rfind("s3://", 0) == 0) {
		if (!exec("INSTALL httpfs;") || !exec("LOAD httpfs;")) {
			log::Warn("httpfs unavailable; plain DuckDB queries against ", prefix, " will fail");
		}
	}

	// todo: don't hardcode dandelion url
	exec("SET plume_url = 'http://localhost:9090/hot/matmul';");

	auto fb = [](bool b) -> std::string {
		if (b) {
			return "true";
		}
		return "false";
	};

	exec("SET plume_projection_pushdown = " + fb(cfg_.converter_config.projection_pushdown) + ";");
	exec("SET plume_filter_pushdown = " + fb(cfg_.converter_config.filter_pushdown) + ";");
	exec("SET plume_early_aggregation = " + fb(cfg_.converter_config.early_aggregation) + ";");
	exec("SET plume_optimize_remote_fetching = " + fb(cfg_.converter_config.optimize_remote_fetching) + ";");
	exec("SET plume_max_splits = " + std::to_string(cfg_.converter_config.max_splits) + ';');
	exec("SET plume_target_rows_per_split = " + std::to_string(cfg_.converter_config.target_rows_per_split) + ';');
	exec("SET plume_max_region_bytes = " + std::to_string(cfg_.converter_config.max_region_bytes) + ';');
	exec("SET plume_coalesce_distance = " + std::to_string(cfg_.converter_config.coalesce_distance) + ';');

	for (auto &rq : resolved_queries) {
		log::Info("[PLUME] ", "Running plume query ", rq.name, "...");
		auto start = std::chrono::steady_clock::now();
		auto result = con.Query(rq.query);
		auto now = std::chrono::steady_clock::now();
		int64_t query_exe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
		if (result->HasError()) {
			log::Error("[PLUME] ", rq.name, " failed: ", result->GetError());
		} else {
			log::Debug(result->ToString());
			log::Info("[PLUME] ", rq.name, " Took ", query_exe_ms, "ms");
			log::Info("[PLUME] ", rq.name, " returned ", result->RowCount(), " rows");
		}
		log::Info("");
		log::Info("[DUCKDB] ", "Running query ", rq.name, " without plume.");
		log::Debug(rq.normal_query);
		start = std::chrono::steady_clock::now();
		result = con.Query(rq.normal_query);
		now = std::chrono::steady_clock::now();
		int64_t normal_query_exe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
		if (result->HasError()) {
			log::Error("[DUCKDB] ", rq.name, " failed: ", result->GetError());
		} else {
			log::Debug(result->ToString());
			log::Info("[DUCKDB] ", rq.name, " Took ", normal_query_exe_ms, "ms without plume (", query_exe_ms,
			          "ms with plume)");
			log::Info("[DUCKDB] ", rq.name, " returned ", result->RowCount(), " rows");
		}
		log::Info("");
	}
}