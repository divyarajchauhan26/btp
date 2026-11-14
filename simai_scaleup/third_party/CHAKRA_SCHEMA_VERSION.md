# Chakra Schema Compatibility

The native Chakra ET importer is validated against the protobuf schema from
`astra-sim/chakra` commit
`bf222b23caf7b6972da17ab7b419d02083c2d2e0`:

`schema/protobuf/et_def.proto`

The importer reads uncompressed, length-delimited Chakra ET protobuf streams
directly and does not require generated protobuf C++ sources or a globally
installed protobuf runtime. It supports metadata versions represented by the
direct `GlobalMetadata.version` field and by the `schema` metadata attribute.
Execution ordering uses `Node.data_deps`, matching Chakra's C++ ET feeder.

Manual compatibility verification also imports Chakra's packaged
`tests/data/feeder_tests_trace.tar.gz` fixture containing a 3,664-node
`chakra.0.et` trace. Gzip-compressed ET files are rejected explicitly.
