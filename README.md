# DuckDB Plume Adapter Extension

This extension integrates plume into DuckDB.

## Usage

The extension adds a new table function `plume_query()` which sends the passed SQL to plume. It then gets
converted into a Dandelion composition and executed on a dandelion server:

``
SELECT * FROM plume_query(<QUERY_TO_PLUME>);
``

### Config

The extension adds two options:
- `plume_url`: URL of the dandelion endpoint
- `plume_timeout`: timeout of a dandelion invocation

Setting an option:
```
SET plume_url = 'http://localhost:8080/hot/matmul';
```

Reading an option:
``` 
SELECT current_setting('plume_timeout');
```

Show all options for this extension:
``` 
SELECT * FROM duckdb_settings() WHERE name LIKE 'plume_%';
```

## Building

Clone the repo recursively:

```sh
git clone --recurse-submodules https://github.com/DarkPhotonBeam/duckdb_plume_extension
```

Run the init script (fetches correct submodule versions and applies patches):
```sh
cd duckdb_plume_extension
./scripts/init.sh
```

Make sure vcpkg is installed running make.
(Consult the [installation instructions](https://vcpkg.io/en/getting-started) if it's not installed)

Run make:
```sh
export VCPKG_TOOLCHAIN_PATH=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
make
```