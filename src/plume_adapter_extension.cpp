#define DUCKDB_EXTENSION_MAIN

#include "plume_adapter_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "plume/catalog/plume_remote.hpp"
#include "plume/catalog/source_scan.hpp"
#include "plume/memory/adapter.hpp"
#include "plume/parser/compile.hpp"

#include <cpr/api.h>
#include <cpr/response.h>
#include <openssl/opensslv.h>

namespace duckdb {

inline void PlumeAdapterScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void PlumeAdapterOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "PlumeAdapter " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

struct PlumeQueryBindData : TableFunctionData {
	std::shared_ptr<const plume::parser::CompiledComposition> cc;
	std::string url;
	int64_t timeout_secs = 0;

	unique_ptr<FunctionData> Copy() const override {
		auto c = make_uniq<PlumeQueryBindData>();
		c->cc = cc;
		c->url = url;
		c->timeout_secs = timeout_secs;
		return std::move(c);
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<PlumeQueryBindData>();
		return cc == o.cc && url == o.url;
	}

	bool SupportStatementCache() const override {
		return false; // todo: could maybe be true, as fetches of data only happen when dandelion executes composition
	}
};

struct PlumeQueryState : GlobalTableFunctionState {
	std::vector<std::vector<plume::dandelion::DataItem>> parsed;
	std::vector<std::unique_ptr<DataChunk>> chunks;
	idx_t chunk_idx = 0;
	idx_t row_offset = 0;
};

static unique_ptr<FunctionData> PlumeQueryBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types, vector<string> &names) {
	auto &info = *static_cast<plume::catalog::PlumeRemoteInfo *>(input.info.get());
	auto sql = StringValue::Get(input.inputs[0]);

	Value url_val, timeout_val;
	context.TryGetCurrentSetting("plume_url", url_val);
	context.TryGetCurrentSetting("plume_timeout", timeout_val);

	plume::parser::ConverterConfig cfg;

	Connection con(*context.db);

	auto res = plume::parser::CompileQuery(con, *info.catalog, sql, "plume_query", cfg);
	if (res.is_error()) {
		throw IOException("CompileQuery failed: %s", res.error_msg());
	}

	auto data = make_uniq<PlumeQueryBindData>();
	data->cc = std::make_shared<plume::parser::CompiledComposition>(std::move(res).unwrap());
	for (auto &col : data->cc->plan->output_schema.columns) {
		names.push_back(col.name);
		return_types.push_back(plume::ToLogicalType(col.type));
	}
	data->url = StringValue::Get(url_val);
	data->timeout_secs = IntegerValue::Get(timeout_val);

	return std::move(data);
}

static void PlumeQueryScan(ClientContext & context, TableFunctionInput & data, DataChunk & output) {
	auto &st = data.global_state->Cast<PlumeQueryState>();
	while (st.chunk_idx < st.chunks.size() && st.row_offset >= st.chunks[st.chunk_idx]->size()) {
		st.chunk_idx++;
		st.row_offset = 0;
	}
	if (st.chunk_idx >= st.chunks.size()) {
		output.SetCardinality(0); // done
		return;
	}
	auto &src = *st.chunks[st.chunk_idx];
	idx_t n = MinValue<idx_t>(STANDARD_VECTOR_SIZE, src.size() - st.row_offset);
	for (idx_t c = 0; c < src.ColumnCount(); c++) {
		VectorOperations::Copy(src.data[c], output.data[c], st.row_offset + n, st.row_offset, 0);
	}
	output.SetCardinality(n);
	st.row_offset += n;
}

static unique_ptr<GlobalTableFunctionState> PlumeQueryInit(ClientContext & context, TableFunctionInitInput & input) {

	auto &bind_data = input.bind_data->Cast<PlumeQueryBindData>();
	auto cc = bind_data.cc;
	auto dandelion_url = bind_data.url;

	// send composition request to dandelion
	auto body_res = plume::dandelion::InvocationBody(cc->composition, cc->table_blocks, cc->remote_info, cc->remote_requests, false);

	if (body_res.is_error()) {
		throw IOException("InvocationBody failed: %s", body_res.error_msg());
	}

	auto body = std::move(body_res).unwrap();

	cpr::Response resp =
		cpr::Post(cpr::Url{bind_data.url}, cpr::Body{reinterpret_cast<const char *>(body.data()), body.size()},
				  cpr::Header{{"Content-Type", "application/octet-stream"}},
				  cpr::Timeout{std::chrono::milliseconds(bind_data.timeout_secs * 1000)});

	if (resp.error) {
		auto msg = "dandelion request to '" + dandelion_url + "' failed: " + resp.error.message;
		throw IOException("Dandelion request failed: %s\n", msg.c_str());
	}
	if (resp.status_code < 200 || resp.status_code >= 300) {
		auto msg = "dandelion returned HTTP " + std::to_string(resp.status_code) + ": " + resp.text;
		throw IOException("Dandelion response error: %s\n", msg.c_str());
	}

	auto bdata = plume::dandelion::BinaryData(resp.text.begin(), resp.text.end());
	auto sets = plume::dandelion::ParseResponseBody(bdata);

	if (sets.is_error()) {
		throw IOException("plume_query: %s", sets.error_msg());
	}

	auto state = make_uniq<PlumeQueryState>();
	state->parsed = std::move(sets).unwrap();
	if (!state->parsed.empty()) {
		for (auto &block : state->parsed[0]) {
			plume::Schema block_schema;
			auto chunks = plume::memory::ImportBlockChunks(block.data.data(), block.data.size(), block_schema);
			if (chunks.is_error()) {
				throw IOException("plume_query: %s", chunks.error_msg());
			}
			for (auto &c : std::move(chunks).unwrap()) {
				state->chunks.push_back(std::move(c));
			}
		}
	}

	return std::move(state);
}


static void LoadInternal(ExtensionLoader & loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("plume_url", "Dandelion URL", LogicalType::VARCHAR, Value("http://127.0.0.1:9090/hot/matmul"));
	config.AddExtensionOption("plume_timeout", "Request timeout in seconds", LogicalType::INTEGER, Value::INTEGER(600));

	auto info = make_shared_ptr<plume::catalog::PlumeRemoteInfo>();
	info->catalog = std::make_shared<plume::catalog::SourceCatalog>();

	TableFunction query_fn("plume_query", {LogicalType::VARCHAR}, PlumeQueryScan, PlumeQueryBind, PlumeQueryInit);
	query_fn.function_info = info;
	loader.RegisterFunction(query_fn);

	TableFunction plume_remote_fn = plume::catalog::GetPlumeRemoteTableFunction(info);
	loader.RegisterFunction(plume_remote_fn);

	// Register another scalar function
	auto plume_adapter_openssl_version_scalar_function = ScalarFunction("plume_adapter_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, PlumeAdapterOpenSSLVersionScalarFun);
	loader.RegisterFunction(plume_adapter_openssl_version_scalar_function);
}

void PlumeAdapterExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string PlumeAdapterExtension::Name() {
	return "plume_adapter";
}

std::string PlumeAdapterExtension::Version() const {
#ifdef EXT_VERSION_PLUME_ADAPTER
	return EXT_VERSION_PLUME_ADAPTER;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(plume_adapter, loader) {
	duckdb::LoadInternal(loader);
}
}
