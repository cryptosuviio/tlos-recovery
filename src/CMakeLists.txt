project(tlosrecoverer)

set(EOSIO_WASM_OLD_BEHAVIOR "Off")
find_package(eosio.cdt)

add_contract( tlosrecoverer tlosrecoverer tlosrecoverer.cpp )
target_include_directories( tlosrecoverer PUBLIC ${CMAKE_SOURCE_DIR}/../include )
target_ricardian_directory( tlosrecoverer ${CMAKE_SOURCE_DIR}/../ricardian )

