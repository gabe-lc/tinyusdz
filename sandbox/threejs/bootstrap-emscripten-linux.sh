emcmake cmake -Bbuild_emcc \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_EXE_LINKER_FLAGS="-s MODULARIZE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web -s EXPORTED_FUNCTIONS=['_malloc','_free'] -s EXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPF32','HEAPU32']"
cd build_emcc
emmake make -j$(sysctl -n hw.ncpu)
