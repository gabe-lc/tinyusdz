// SPDX-License-Identifier: Apache 2.0
// Copyright 2019 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertinament Inc.

#include <algorithm>
#include <atomic>
//#include <cassert>
#include <cctype>  // std::tolower
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

#include "usdLux.hh"

#ifndef __wasi__
#include <thread>
#endif

#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "image-loader.hh"
#include "integerCoding.h"
#include "io-util.hh"
#include "lz4-compression.hh"
#include "zstd-compression.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "tiny-format.hh"
#include "tinyusdz.hh"
#include "layer.hh"
#include "usda-reader.hh"
#include "usdc-reader.hh"
#include "mmap-array-ref.hh"
#include "value-pprint.hh"


#include "common-macros.inc"

namespace tinyusdz {

// Global flag to control DCOUT output. Defaults to false to suppress flood of output.
// Set to true via TINYUSDZ_ENABLE_DCOUT environment variable.
bool g_enable_dcout_output = false;

// constexpr auto kTagUSDA = "[USDA]";
// constexpr auto kTagUSDC = "[USDC]";
// constexpr auto kTagUSDZ = "[USDZ]";

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) {         \
    (*err) += s;     \
  }
//#define PushWarn(s) if (warn) { (*warn) += s; }

// Helper function to format magic header bytes for error messages
static std::string FormatMagicHeader(const uint8_t *addr, const size_t length, size_t max_bytes = 16) {
  if (!addr || length == 0) {
    return "(empty)";
  }
  
  std::string result = "0x";
  size_t bytes_to_show = std::min(length, max_bytes);
  
  for (size_t i = 0; i < bytes_to_show; i++) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", addr[i]);
    result += hex;
    if (i < bytes_to_show - 1) {
      result += " ";
    }
  }
  
  if (length > max_bytes) {
    result += "...";
  }
  
  return result;
}

bool LoadUSDCFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (stage == nullptr) {
    if (err) {
      (*err) = "null pointer for `stage` argument.\n";
    }
    return false;
  }

  bool swap_endian = false;  // @FIXME

  size_t max_length;

  // 32bit env
  if (sizeof(void *) == 4) {
    if (options.max_memory_limit_in_mb > 4096) {  // exceeds 4GB
      max_length = std::numeric_limits<uint32_t>::max();
    } else {
      max_length =
          size_t(1024) * size_t(1024) * size_t(options.max_memory_limit_in_mb);
    }
  } else {
    // TODO: Set hard limit?
    max_length =
        size_t(1024) * size_t(1024) * size_t(options.max_memory_limit_in_mb);
  }

  DCOUT("Max length = " << max_length);

  if (length > max_length) {
    if (err) {
      (*err) += "USDC data [" + filename +
                "] is too large(size = " + std::to_string(length) +
                ", which exceeds memory limit " + std::to_string(max_length) +
                ".\n";
    }

    return false;
  }

  StreamReader sr(addr, length, swap_endian);

  usdc::USDCReaderConfig config;
  config.numThreads = options.num_threads;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.kMaxAllowedMemoryInMB = size_t(options.max_memory_limit_in_mb);
  config.mmap_zero_copy = options.mmap_zero_copy;
  usdc::USDCReader reader(&sr, config);

  if (options.progress_callback) {
    reader.SetProgressCallback(options.progress_callback, options.progress_userptr);
  }

  if (!reader.ReadUSDC()) {
    if (warn) {
      (*warn) = reader.GetWarning();
    }

    if (err) {
      (*err) = reader.GetError();
    }
    return false;
  }

  DCOUT("Loaded USDC file.");

  // Reconstruct `Stage`(scene) object
  {
    if (!reader.ReconstructStage(stage)) {
      DCOUT("Failed to reconstruct Stage from Crate.");
      if (warn) {
        (*warn) = reader.GetWarning();
      }

      if (err) {
        (*err) = reader.GetError();
      }
      return false;
    }

    // Set mmap data source on Stage for zero-copy array access
    if (options.mmap_zero_copy) {
      stage->set_mmap_source(MMapDataSource(addr, length));
    }
  }

  if (warn) {
    (*warn) = reader.GetWarning();
  }

  // Reconstruct OK but may have some error.
  // TODO(syoyo): Return false in strict mode.
  if (err) {
    DCOUT(reader.GetError());
    (*err) = reader.GetError();
  }

  DCOUT("Reconstructed Stage from USDC file.");

  return true;
}

bool LoadUSDCFromFile(const std::string &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);

  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;
    
    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false; 
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDCFromMemory(handle.addr, size_t(handle.size), filepath, stage, warn,
                              err, options);

    {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;

  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = 1024 * 1024 * size_t(options.max_memory_limit_in_mb);
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      if (err) {
        (*err) += "File not found or failed to read : \"" + filepath + "\"\n";
      }

      return false;
    }

    DCOUT("File size: " + std::to_string(data.size()) + " bytes.");

    if (data.size() < (11 * 8)) {
      // ???
      if (err) {
        (*err) += "File size too short. Looks like this file is not a USDC : \"" +
                  filepath + "\"\n";
      }
      return false;
    }

    return LoadUSDCFromMemory(data.data(), data.size(), filepath, stage, warn,
                              err, options);
  }
}

namespace {

static std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of('.') != std::string::npos)
    return filename.substr(filename.find_last_of('.') + 1);
  return "";
}

static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

namespace {

struct USDZAssetInfo {
  std::string filename;
  size_t byte_begin;
  size_t byte_end;
};

bool ParseUSDZHeader(const uint8_t *addr, const size_t length,
                     std::vector<USDZAssetInfo> *assets, std::string *warn,
                     std::string *err) {
  (void)warn;

  if (!addr) {
    if (err) {
      (*err) += "null for `addr` argument.\n";
    }
    return false;
  }

  if (length < (11 * 8) + 30) {  // 88 for USDC header, 30 for ZIP header
    // ???
    if (err) {
      (*err) += "File size too short. Looks like this file is not a USDZ\n";
    }
    return false;
  }

  size_t offset = 0;
  while ((offset + 30) < length) {
    //
    // PK zip format:
    // https://users.cs.jmu.edu/buchhofp/forensics/formats/pkzip.html
    //
    std::vector<char> local_header(30);
    memcpy(local_header.data(), addr + offset, 30);

    // Check signagure(first 4 bytes)
    // Must be \x50\x4b\x03\x04
    if ((local_header[0] == 0x50) && (local_header[1] == 0x4b) &&
        (local_header[2] == 0x03) && (local_header[3] == 0x04)) {
      // ok

      // TODO: Check other header info(version, flags, crc32)
    } else {
      if (offset == 0) {
        // Invalid header found.
        if (err) {
          (*err) += "PKZIP header not found.\n";
        }
        return false;
      } else {
        // not a local(global?) header
        // Maybe near to the end of file.
        break;
      }
    }

    offset += 30;

    // read in the variable name
    uint16_t name_len;
    memcpy(&name_len, &local_header[26], sizeof(uint16_t));
    if ((offset + name_len) > length) {
      if (err) {
        (*err) += "Invalid ZIP data\n";
      }
      return false;
    }

    std::string varname(name_len, ' ');
    memcpy(&varname[0], addr + offset, name_len);

    offset += name_len;

    // read in the extra field
    uint16_t extra_field_len;
    memcpy(&extra_field_len, &local_header[28], sizeof(uint16_t));
    if (extra_field_len > 0) {
      if (offset + extra_field_len > length) {
        if (err) {
          (*err) += "Invalid extra field length in ZIP data\n";
        }
        return false;
      }
    }

    offset += extra_field_len;

    // In usdz, data must be aligned at 64bytes boundary.
    if ((offset % 64) != 0) {
      if (err) {
        (*err) += "Data offset must be mulitple of 64bytes for USDZ, but got " +
                  std::to_string(offset) + ".\n";
      }
      return false;
    }

    uint16_t compr_method = *reinterpret_cast<uint16_t *>(&local_header[0] + 8);
    // uint32_t compr_bytes = *reinterpret_cast<uint32_t*>(&local_header[0]+18);
    uint32_t uncompr_bytes;
    memcpy(&uncompr_bytes, &local_header[22], sizeof(uncompr_bytes));

    // USDZ only supports uncompressed ZIP
    if (compr_method != 0) {
      if (err) {
        (*err) += "Compressed ZIP is not supported for USDZ\n";
      }
      return false;
    }

    if (assets) {
      USDZAssetInfo info;
      DCOUT("USDZasset[" << assets->size() << "] " << varname << ", byte_begin " << offset << ", length " << uncompr_bytes << "\n");
      info.filename = varname;
      info.byte_begin = offset;
      info.byte_end = offset + uncompr_bytes;

      assets->push_back(info);
    }

    offset += uncompr_bytes;
  }

  return true;
}

}  // namespace

bool LoadUSDZFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  std::vector<USDZAssetInfo> assets;
  if (!ParseUSDZHeader(addr, length, &assets, warn, err)) {
    return false;
  }

#ifdef TINYUSDZ_LOCAL_DEBUG_PRINT
  for (size_t i = 0; i < assets.size(); i++) {
    DCOUT("[" << i << "] " << assets[i].filename << " : byte range ("
              << assets[i].byte_begin << ", " << assets[i].byte_end << ")");
  }
#endif

  int32_t usdc_index = -1;
  int32_t usda_index = -1;
  {
    bool warned = false;  // to report single warning message.
    for (size_t i = 0; i < assets.size(); i++) {
      std::string ext = str_tolower(GetFileExtension(assets[i].filename));
      if (ext.compare("usdc") == 0) {
        if ((usdc_index > -1) && (!warned)) {
          if (warn) {
            (*warn) +=
                "Multiple USDC files were found in USDZ. Use the first found "
                "one: " +
                assets[size_t(usdc_index)].filename + "]\n";
          }
          warned = true;
        }

        if (usdc_index == -1) {
          usdc_index = int32_t(i);
        }
      } else if (ext.compare("usda") == 0) {
        if ((usda_index > -1) && (!warned)) {
          if (warn) {
            (*warn) +=
                "Multiple USDA files were found in USDZ. Use the first found "
                "one: " +
                assets[size_t(usda_index)].filename + "]\n";
          }
          warned = true;
        }
        if (usda_index == -1) {
          usda_index = int32_t(i);
        }
      }
    }
  }

  if ((usdc_index == -1) && (usda_index == -1)) {
    if (err) {
      (*err) += "Neither USDC nor USDA file found in USDZ\n";
    }
    return false;
  }

  if ((usdc_index >= 0) && (usda_index >= 0)) {
    // Per USDZ spec, the first file in the archive is the entry layer. The
    // earlier code here unconditionally preferred USDC, which drops the
    // top-level .usda in assembly USDZs that reference sibling .usdc files
    // (the .usdc gets used as the root and the assembly never gets read).
    // Honor archive ordering: whichever of usda/usdc has the lower index is
    // the entry layer.
    if (usda_index < usdc_index) {
      if (warn) {
        (*warn) +=
            "Both USDA and USDC file found. First in archive is USDA, using [" +
            assets[size_t(usda_index)].filename + "]\n";
      }
      usdc_index = -1;
    } else {
      if (warn) {
        (*warn) +=
            "Both USDA and USDC file found. First in archive is USDC, using [" +
            assets[size_t(usdc_index)].filename + "]\n";
      }
      usda_index = -1;
    }
  }

  if (usdc_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usdc_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usdc_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usdc_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usdc_addr = addr + start_addr_offset;
    bool ret = LoadUSDCFromMemory(usdc_addr, usdc_size, filename, stage, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        (*err) += "Failed to load USDC: [" + filename + "].\n";
      }

      return false;
    }
  } else if (usda_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usda_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usda_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usda_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usda_addr = addr + start_addr_offset;
    bool ret = LoadUSDAFromMemory(usda_addr, usda_size, filename, stage, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        (*err) += "Failed to load USDA: [" + filename + "].\n";
      }

      return false;
    }
  }


  return true;
}

bool LoadUSDZFromFile(const std::string &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  // <filename, byte_begin, byte_end>
  std::vector<std::tuple<std::string, size_t, size_t>> assets;

  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);


  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;
    
    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false; 
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDZFromMemory(handle.addr, size_t(handle.size), filepath, stage, warn,
                              err, options);

    {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;
  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = 1024 * 1024 * size_t(options.max_memory_limit_in_mb);
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      return false;
    }

    if (data.size() < (11 * 8) + 30) {  // 88 for USDC header, 30 for ZIP header
      // ???
      if (err) {
        (*err) += "File size too short. Looks like this file is not a USDZ : \"" +
                  filepath + "\"\n";
      }
      return false;
    }

    return LoadUSDZFromMemory(data.data(), data.size(), filepath, stage, warn,
                              err, options);
  }
}

#ifdef _WIN32
bool LoadUSDZFromFile(const std::wstring &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  std::string filename = io::WcharToUTF8(_filename);
  return LoadUSDZFromFile(filename, stage, warn, err, options);
}
#endif

bool LoadUSDAFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &base_dir, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (addr == nullptr) {
    if (err) {
      (*err) = "null pointer for `addr` argument.\n";
    }
    return false;
  }

  if (stage == nullptr) {
    if (err) {
      (*err) = "null pointer for `stage` argument.\n";
    }
    return false;
  }

  tinyusdz::StreamReader sr(addr, length, /* swap endian */ false);
  tinyusdz::usda::USDAReader reader(&sr);

  tinyusdz::usda::USDAReaderConfig config;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.allow_unknown_apiSchema = !options.strict_apiSchema_check;
  config.max_memory_limit_in_mb = size_t(options.max_memory_limit_in_mb);
  // MaterialX validation options
  config.strict_mtlx_check = options.strict_mtlx_check;
  config.validate_mtlx_info_id = options.validate_mtlx_info_id;
  config.validate_mtlx_connection_types = options.validate_mtlx_connection_types;
  config.validate_mtlx_connection_targets = options.validate_mtlx_connection_targets;
  config.validate_mtlx_duplicate_names = options.validate_mtlx_duplicate_names;
  config.validate_mtlx_index_bounds = options.validate_mtlx_index_bounds;
  reader.set_reader_config(config);

  if (options.progress_callback) {
    reader.SetProgressCallback(options.progress_callback, options.progress_userptr);
  }

  reader.SetBaseDir(base_dir);
  reader.set_filename(base_dir);  // Pass filename for error context display

  {
    bool ret = reader.Read();

    if (!ret) {
      if (err) {
        (*err) += "Failed to parse USDA\n";
        (*err) += reader.GetError();
      }

      return false;
    }
  }

  {
    bool ret = reader.ReconstructStage();
    if (!ret) {
      if (err) {
        (*err) += "Failed to reconstruct Stage from USDA:\n";
        (*err) += reader.GetError() + "\n";
      }
      return false;
    }
  }

  (*stage) = reader.GetStage();

  if (warn) {
    (*warn) += reader.GetWarning();
  }

  return true;
}

bool LoadUSDAFromFile(const std::string &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(_filename);

  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;
    
    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false; 
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDAFromMemory(handle.addr, size_t(handle.size), filepath, stage, warn,
                              err, options);

    {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;
  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = 1024 * 1024 * size_t(options.max_memory_limit_in_mb);
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      if (err) {
        (*err) += "File not found or failed to read : \"" + filepath + "\"\n";
      }
    }

    return LoadUSDAFromMemory(data.data(), data.size(), filepath, stage, warn,
                              err, options);
  }
}

bool LoadUSDFromFile(const std::string &_filename, Stage *stage,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options) {
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(_filename);

  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;
    
    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false; 
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDFromMemory(handle.addr, size_t(handle.size), filepath, stage, warn,
                              err, options);

    {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;
  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = 1024 * 1024 * size_t(options.max_memory_limit_in_mb);
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      return false;
    }

    return LoadUSDFromMemory(data.data(), data.size(), base_dir, stage, warn, err,
                             options);
  }
}

bool LoadUSDFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &base_dir, Stage *stage,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options) {
  // Check for zstd-compressed data first (file-level compression)
  if (IsZstdCompressed(addr, length)) {
    DCOUT("Detected as zstd-compressed USD.");
#ifdef TINYUSDZ_WITH_ZSTD_COMPRESSION
    // Get decompressed size for memory budget check
    std::string zstd_err;
    size_t decompressed_size = ZstdCompression::GetDecompressedSize(addr, length, &zstd_err);
    if (decompressed_size == 0) {
      if (err) {
        (*err) += "Failed to get zstd decompressed size: " + zstd_err + "\n";
      }
      return false;
    }

    // Check against memory budget
    size_t max_length = size_t(1024) * size_t(1024) * size_t(options.max_memory_limit_in_mb);
    if (decompressed_size > max_length) {
      if (err) {
        (*err) += "Decompressed USD size (" + std::to_string(decompressed_size) +
                  " bytes) exceeds memory limit (" + std::to_string(max_length) + " bytes)\n";
      }
      return false;
    }

    // Decompress
    std::vector<uint8_t> decompressed_data;
    if (!ZstdCompression::Decompress(addr, length, &decompressed_data, &zstd_err)) {
      if (err) {
        (*err) += "Failed to decompress zstd data: " + zstd_err + "\n";
      }
      return false;
    }

    // Recursively call LoadUSDFromMemory with decompressed data
    return LoadUSDFromMemory(decompressed_data.data(), decompressed_data.size(),
                            base_dir, stage, warn, err, options);
#else
    if (err) {
      (*err) += "zstd-compressed USD file detected, but zstd compression support is not enabled. "
                "Rebuild with TINYUSDZ_WITH_ZSTD_COMPRESSION=ON.\n";
    }
    return false;
#endif
  }

  if (IsUSDC(addr, length)) {
    DCOUT("Detected as USDC.");
    return LoadUSDCFromMemory(addr, length, base_dir, stage, warn, err,
                              options);
  } else if (IsUSDA(addr, length)) {
    DCOUT("Detected as USDA.");
    return LoadUSDAFromMemory(addr, length, base_dir, stage, warn, err,
                              options);
  } else if (IsUSDZ(addr, length)) {
    DCOUT("Detected as USDZ.");
    return LoadUSDZFromMemory(addr, length, base_dir, stage, warn, err,
                              options);
  } else {
    if (err) {
      (*err) += "Couldn't determine USD format(USDA/USDC/USDZ). ";
      (*err) += "Found magic header: " + FormatMagicHeader(addr, length, 8) + ", ";
      (*err) += "expected: \"#usda 1.0\" (0x23 75 73 64 61 20 31 2e 30) for USDA, ";
      (*err) += "\"PXR-USDC\" (0x50 58 52 2d 55 53 44 43) for USDC, ";
      (*err) += "or ZIP signature (0x50 4b 03 04) for USDZ.\n";
    }
    return false;
  }
}

bool ReadUSDZAssetInfoFromMemory(const uint8_t *addr, const size_t length, const bool asset_on_memory, USDZAsset *asset,
  std::string *warn, std::string *err) {

  if (!asset) {
    return false;
  }

  std::vector<USDZAssetInfo> assetInfos;
  if (!ParseUSDZHeader(addr, length, &assetInfos, warn, err)) {
    return false;
  }

  for (size_t i = 0; i < assetInfos.size(); i++) {
    if (assetInfos[i].byte_begin > length) {
      if (err) {
        (*err) += "Invalid byte begin offset in USDZ asset header.";
      }
      return false;
    }
    if (assetInfos[i].byte_end > length) {
      if (err) {
        (*err) += "Invalid byte end offset in USDZ asset header.";
      }
      return false;
    }
    // Assume same filename does not exist.
    asset->asset_map[assetInfos[i].filename] = std::make_pair(assetInfos[i].byte_begin, assetInfos[i].byte_end);
  }

  if (asset_on_memory) {
    asset->data.clear();
    asset->addr = addr;
    asset->size = length;
  } else {
    // copy content
    asset->data.resize(length);
    memcpy(asset->data.data(), addr, length);
    asset->addr = nullptr;
    asset->size = 0;
  }

  return true;
}

bool ReadUSDZAssetInfoFromFile(const std::string &_filename, USDZAsset *asset,
  std::string *warn, std::string *err, size_t max_memory_limit_in_mb) {

  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(_filename);

  std::vector<uint8_t> data;
  size_t max_bytes = 1024ull * 1024ull * max_memory_limit_in_mb;
  if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                         /* userdata */ nullptr)) {
    return false;
  }

  return ReadUSDZAssetInfoFromMemory(data.data(), data.size(), /* asset_on_memory */false, asset, warn, err);

}

//
// File type detection
//

bool IsUSDA(const std::string &filename) {
  // TODO: Read first few bytes and check the magic number.
  //
  std::vector<uint8_t> data;
  std::string err;
  // 12 = enough storage for "#usda 1.0"
  if (!io::ReadFileHeader(&data, &err, filename, 12,
                          /* userdata */ nullptr)) {
    // TODO: return `err`
    return false;
  }

  return IsUSDA(data.data(), data.size());
}

bool IsUSDA(const uint8_t *addr, const size_t length) {
  if (length < 9) {
    return false;
  }
  const char header[9 + 1] = "#usda 1.0";

  if (memcmp(header, addr, 9) == 0) {
    return true;
  }

  return false;
}

bool IsUSDC(const std::string &filename) {
  // TODO: Read first few bytes and check the magic number.
  //
  std::vector<uint8_t> data;
  std::string err;
  // 88 bytes should enough
  if (!io::ReadFileHeader(&data, &err, filename, /* header bytes */ 88,
                          /* userdata */ nullptr)) {
    return false;
  }

  return IsUSDC(data.data(), data.size());
}

bool IsUSDC(const uint8_t *addr, const size_t length) {
  // must be 88bytes or more
  if (length < 88) {
    return false;
  }
  const char header[8 + 1] = "PXR-USDC";

  if (memcmp(header, addr, 8) == 0) {
    return true;
  }

  return false;
}

bool IsUSDZ(const std::string &filename) {
  // TODO: Read first few bytes and check the magic number.
  //
  std::vector<uint8_t> data;
  std::string err;
  // 256 bytes may be enough.
  if (!io::ReadFileHeader(&data, &err, filename, 256,
                          /* userdata */ nullptr)) {
    return false;
  }

  return IsUSDZ(data.data(), data.size());
}

bool IsUSDZ(const uint8_t *addr, const size_t length) {
  std::string warn;
  std::string err;

  return ParseUSDZHeader(addr, length, /* [out] assets */ nullptr, &warn, &err);
}

bool IsZstdCompressed(const uint8_t *addr, const size_t length) {
  return ZstdCompression::IsZstdCompressed(addr, length);
}

bool IsUSD(const std::string &filename, std::string *detected_format) {
  if (IsUSDA(filename)) {
    if (detected_format) {
      (*detected_format) = "usda";
    }
    return true;
  }

  if (IsUSDC(filename)) {
    if (detected_format) {
      (*detected_format) = "usdc";
    }
    return true;
  }

  if (IsUSDZ(filename)) {
    if (detected_format) {
      (*detected_format) = "usdz";
    }
    return true;
  }

  return false;
}

bool IsUSD(const uint8_t *addr, const size_t length, std::string *detected_format) {
  if (IsUSDA(addr, length)) {
    if (detected_format) {
      (*detected_format) = "usda";
    }
    return true;
  }

  if (IsUSDC(addr, length)) {
    if (detected_format) {
      (*detected_format) = "usdc";
    }
    return true;
  }

  if (IsUSDZ(addr, length)) {
    if (detected_format) {
      (*detected_format) = "usdz";
    }
    return true;
  }

  return false;
}

bool LoadUSDCLayerFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Layer *layer,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (layer == nullptr) {
    if (err) {
      (*err) = "null pointer for `layer` argument.\n";
    }
    return false;
  }

  bool swap_endian = false;  // @FIXME

  size_t max_length;

  // 32bit env
  if (sizeof(void *) == 4) {
    if (options.max_memory_limit_in_mb > 4096) {  // exceeds 4GB
      max_length = std::numeric_limits<uint32_t>::max();
    } else {
      max_length =
          size_t(1024) * size_t(1024) * size_t(options.max_memory_limit_in_mb);
    }
  } else {
    // TODO: Set hard limit?
    max_length =
        size_t(1024) * size_t(1024) * size_t(options.max_memory_limit_in_mb);
  }

  DCOUT("Max length = " << max_length);

  if (length > max_length) {
    if (err) {
      (*err) += "USDC data [" + filename +
                "] is too large(size = " + std::to_string(length) +
                ", which exceeds memory limit " + std::to_string(max_length) +
                ".\n";
    }

    return false;
  }

  StreamReader sr(addr, length, swap_endian);

  usdc::USDCReaderConfig config;
  config.numThreads = options.num_threads;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.allow_unknown_apiSchemas = !options.strict_apiSchema_check;
  usdc::USDCReader reader(&sr, config);

  if (!reader.ReadUSDC()) {
    if (warn) {
      (*warn) = reader.GetWarning();
    }

    if (err) {
      (*err) = reader.GetError();
    }
    return false;
  }

  DCOUT("Loaded USDC file.");

  {
    if (!reader.get_as_layer(layer)) {
      DCOUT("Failed to reconstruct Layer from Crate.");
      if (warn) {
        (*warn) = reader.GetWarning();
      }

      if (err) {
        (*err) = reader.GetError();
      }
      return false;
    }
  }

  if (warn) {
    (*warn) = reader.GetWarning();
  }

  // Reconstruct OK but may have some error.
  // TODO(syoyo): Return false in strict mode.
  if (err) {
    DCOUT(reader.GetError());
    (*err) = reader.GetError();
  }

  DCOUT("Reconstructed Stage from USDC file.");

  return true;
}

bool LoadUSDALayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &asset_name, Layer *dst_layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options) {

  // TODO: options
  (void)options;

  if (!addr) {
    if (err) {
      (*err) += "addr arg is nullptr.\n";
    }
    return false;
  }

  if (length < 9) {
    if (err) {
      (*err) += "Input too short.\n";
    }
    return false;
  }

  if (!dst_layer) {
    if (err) {
      (*err) += "dst_layher arg is nullptr.\n";
    }
    return false;
  }

  tinyusdz::StreamReader sr(addr, length, /* swap endian */ false);
  tinyusdz::usda::USDAReader reader(&sr);

  tinyusdz::usda::USDAReaderConfig config;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  // MaterialX validation options
  config.strict_mtlx_check = options.strict_mtlx_check;
  config.validate_mtlx_info_id = options.validate_mtlx_info_id;
  config.validate_mtlx_connection_types = options.validate_mtlx_connection_types;
  config.validate_mtlx_connection_targets = options.validate_mtlx_connection_targets;
  config.validate_mtlx_duplicate_names = options.validate_mtlx_duplicate_names;
  config.validate_mtlx_index_bounds = options.validate_mtlx_index_bounds;
  reader.set_reader_config(config);

  uint32_t load_states = static_cast<uint32_t>(tinyusdz::LoadState::Toplevel);

  bool as_primspec = true;

  {
    bool ret = reader.read(load_states, as_primspec);

    if (!ret) {
      if (err) {
        (*err) += "Failed to parse USDA: " + asset_name + "\n";
        (*err) += reader.get_error() + "\n";
      }
      return false;
    }
  }

  tinyusdz::Layer layer;
  bool ret = reader.get_as_layer(&layer);
  if (!ret) {
    if (err) {
      (*err) += reader.get_error();
    }
    return false;
  }

  if (warn) {
    if (reader.get_warning().size()) {
      (*warn) += reader.get_warning();
    }
  }

  (*dst_layer) = std::move(layer);

  return true;
}

bool LoadUSDZLayerFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Layer *layer,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (layer == nullptr) {
    if (err) {
      (*err) = "null pointer for `layer` argument.\n";
    }
    return false;
  }

  std::vector<USDZAssetInfo> assets;
  if (!ParseUSDZHeader(addr, length, &assets, warn, err)) {
    return false;
  }

#ifdef TINYUSDZ_LOCAL_DEBUG_PRINT
  for (size_t i = 0; i < assets.size(); i++) {
    DCOUT("[" << i << "] " << assets[i].filename << " : byte range ("
              << assets[i].byte_begin << ", " << assets[i].byte_end << ")");
  }
#endif

  int32_t usdc_index = -1;
  int32_t usda_index = -1;
  {
    bool warned = false;  // to report single warning message.
    for (size_t i = 0; i < assets.size(); i++) {
      std::string ext = str_tolower(GetFileExtension(assets[i].filename));
      if (ext.compare("usdc") == 0) {
        if ((usdc_index > -1) && (!warned)) {
          if (warn) {
            (*warn) +=
                "Multiple USDC files were found in USDZ. Use the first found "
                "one: " +
                assets[size_t(usdc_index)].filename + "]\n";
          }
          warned = true;
        }

        if (usdc_index == -1) {
          usdc_index = int32_t(i);
        }
      } else if (ext.compare("usda") == 0) {
        if ((usda_index > -1) && (!warned)) {
          if (warn) {
            (*warn) +=
                "Multiple USDA files were found in USDZ. Use the first found "
                "one: " +
                assets[size_t(usda_index)].filename + "]\n";
          }
          warned = true;
        }
        if (usda_index == -1) {
          usda_index = int32_t(i);
        }
      }
    }
  }

  if ((usdc_index == -1) && (usda_index == -1)) {
    if (err) {
      (*err) += "Neither USDC nor USDA file found in USDZ\n";
    }
    return false;
  }

  if ((usdc_index >= 0) && (usda_index >= 0)) {
    // Per USDZ spec, the first file in the archive is the entry layer. See
    // LoadUSDZFromMemory for full rationale — same bug, same fix.
    if (usda_index < usdc_index) {
      if (warn) {
        (*warn) +=
            "Both USDA and USDC file found. First in archive is USDA, using [" +
            assets[size_t(usda_index)].filename + "]\n";
      }
      usdc_index = -1;
    } else {
      if (warn) {
        (*warn) +=
            "Both USDA and USDC file found. First in archive is USDC, using [" +
            assets[size_t(usdc_index)].filename + "]\n";
      }
      usda_index = -1;
    }
  }

  if (usdc_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usdc_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usdc_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usdc_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usdc_addr = addr + start_addr_offset;
    bool ret = LoadUSDCLayerFromMemory(usdc_addr, usdc_size, filename, layer, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        (*err) += "Failed to load USDC: [" + filename + "].\n";
      }

      return false;
    }
  } else if (usda_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usda_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usda_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usda_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usda_addr = addr + start_addr_offset;
    bool ret = LoadUSDALayerFromMemory(usda_addr, usda_size, filename, layer, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        (*err) += "Failed to load USDA: [" + filename + "].\n";
      }

      return false;
    }
  }

  return true;
}


// Copy assetresolver state to all PrimSpec in the tree.
static bool PropagateAssetResolverState(uint32_t depth, PrimSpec &ps,
                                 const std::string &cwp,
                                 const std::vector<std::string> &search_paths) {
  if (depth > (1024 * 1024 * 512)) {
    return false;
  }

  if (depth == 0) {
    DCOUT("current_working_path: " << cwp);
    DCOUT("search_paths: " << search_paths);
  }

  ps.set_asset_resolution_state(cwp, search_paths);

  for (auto &child : ps.children()) {
    if (!PropagateAssetResolverState(depth + 1, child, cwp, search_paths)) {
      return false;
    }
  }

    return true;
}

bool LoadLayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &asset_name, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options) {

  bool ret{false};

  if (IsUSDC(addr, length)) {
    DCOUT("Detected as USDC.");
#if 1
    ret = LoadUSDCLayerFromMemory(addr, length, asset_name, layer, warn, err,
                              options);
#else
    if (err) {
      (*err) += "TODO: Load USDC as Layer is not implemented yet.\n";
    }
    return false;
#endif
  } else if (IsUSDA(addr, length)) {
    DCOUT("Detected as USDA.");
    ret = LoadUSDALayerFromMemory(addr, length, asset_name, layer, warn, err,
                              options);
  } else if (IsUSDZ(addr, length)) {
    DCOUT("Detected as USDZ.");
#if 1
    // TODO: asset
    return LoadUSDZLayerFromMemory(addr, length, asset_name, layer, warn, err,
                              options);
#else
    if (err) {
      (*err) += "TODO: Load USDZ as Layer is not implemented yet.\n";
    }
    return false;
#endif
  } else {
    if (err) {
      (*err) += "Couldn't determine USD format(USDA/USDC/USDZ). ";
      (*err) += "Found magic header: " + FormatMagicHeader(addr, length, 8) + ", ";
      (*err) += "expected: \"#usda 1.0\" (0x23 75 73 64 61 20 31 2e 30) for USDA, ";
      (*err) += "\"PXR-USDC\" (0x50 58 52 2d 55 53 44 43) for USDC, ";
      (*err) += "or ZIP signature (0x50 4b 03 04) for USDZ.\n";
    }
    return false;
  }

  if (ret) {
    std::vector<std::string> search_paths; // empty
    std::string basedir = io::GetBaseDir(asset_name);
    // Save current working path to each PrimSpec in the layer
    // for the subsequent composition operation.
    for (auto &root_ps : layer->primspecs()) {
      PropagateAssetResolverState(0, root_ps.second, basedir, search_paths);
    }
  }

  return ret;
}

bool LoadLayerFromFile(const std::string &_filename, Layer *stage,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options) {

  if (_filename.empty()) {
    PUSH_ERROR_AND_RETURN("Input filename is empty.");
  }

  // TODO: Use AssetResolutionResolver.
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(_filename);

  std::vector<uint8_t> data;
  size_t max_bytes = 1024 * 1024 * size_t(options.max_memory_limit_in_mb);
  if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                         /* userdata */ nullptr)) {
    return false;
  }

  return LoadLayerFromMemory(data.data(), data.size(), filepath, stage, warn, err,
                           options);
}

bool LoadLayerFromAsset(AssetResolutionResolver &resolver, const std::string &resolved_asset_name, Layer *layer,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options) {

  if (resolved_asset_name.empty()) {
    PUSH_ERROR_AND_RETURN("Input asset name is empty.");
  }

  resolver.set_max_asset_bytes_in_mb(options.max_allowed_asset_size_in_mb);

  Asset asset;
  if (!resolver.open_asset(resolved_asset_name, resolved_asset_name, &asset, warn, err)) {
    PUSH_ERROR_AND_RETURN(fmt::format("Failed to open asset `{}`.", resolved_asset_name));
  }

  return LoadLayerFromMemory(asset.data(), asset.size(), resolved_asset_name, layer, warn, err,
                           options);
}

// Collapse "./" and "../" segments in a USDZ-internal path. Used by
// USDZResolveAsset so that `references = @../../Materials/.../MI_*.usd@`
// authored relative to a deep material/mesh layer can hit the (flat,
// archive-rooted) asset_map. Treats any leading ".." segments that try to
// escape the archive root as a hard fail — they cannot resolve to a real
// archive entry. Pure string transform, no filesystem access.
static bool NormalizeUSDZPath(const std::string &in, std::string *out) {
  if (!out) return false;
  std::vector<std::string> stack;
  size_t i = 0;
  while (i < in.size()) {
    size_t j = in.find('/', i);
    std::string seg = (j == std::string::npos) ? in.substr(i) : in.substr(i, j - i);
    if (seg.empty() || seg == ".") {
      // skip
    } else if (seg == "..") {
      if (stack.empty()) {
        // escape attempt — caller treats as not-found.
        return false;
      }
      stack.pop_back();
    } else {
      stack.push_back(std::move(seg));
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  std::string joined;
  for (size_t k = 0; k < stack.size(); k++) {
    if (k) joined += '/';
    joined += stack[k];
  }
  *out = std::move(joined);
  return true;
}

int USDZResolveAsset(const char *asset_name, const std::vector<std::string> &search_paths, std::string *resolved_asset_name, std::string *err, void *userdata) {

  DCOUT("Resolve asset: " << asset_name);

  if (!userdata) {
    if (err) {
      (*err) += "`userdata` must be non-null.\n";
    }
    return -2;
  }

  if (!asset_name) {
    if (err) {
      (*err) += "`asset_name` must be non-null.\n";
    }
    return -2;
  }

  if (!resolved_asset_name) {
    if (err) {
      (*err) += "`resolved_asset_name` must be non-null.\n";
    }
    return -2;
  }

  std::string asset_path = asset_name;

  // Remove relative path prefix './'
  if (tinyusdz::startsWith(asset_path, "./")) {
    asset_path = tinyusdz::removePrefix(asset_path, "./");
  }

  const USDZAsset *passet = reinterpret_cast<const USDZAsset *>(userdata);

  // 1) Literal lookup (handles refs authored relative to the archive root,
  //    such as the root layer's own references to sibling files).
  if (passet->asset_map.count(asset_path)) {
    DCOUT("Resolved asset: " << asset_name << " as " << asset_path);
    (*resolved_asset_name) = asset_path;
    return 0;
  }

  // 2) Try each caller-supplied search path as a prefix. Asset references in
  //    USD are resolved relative to the *layer* that authored them — e.g. a
  //    material at `Assets/Game/Foo/Materials/MI_Bar.usd` saying
  //    `inputs:file = @Textures/Bar_BaseColor.png@` should resolve against the
  //    material's directory, not the archive root. The composition pipeline
  //    propagates each layer's cwd into `search_paths`; without using it here,
  //    Unreal-style nested layouts would silently fail to resolve any texture
  //    that isn't in the archive root. We normalize separators to `/` (USDZ
  //    spec uses forward slashes) and collapse `./` and `../` segments so
  //    deep-layer refs like `prepend references = @../../Materials/.../MI_*.usd@`
  //    (Unreal decal/curtain/door assets) resolve against the flat archive
  //    asset_map. Any `..` chain that escapes the archive root is a hard miss.
  for (const auto &raw_prefix : search_paths) {
    if (raw_prefix.empty()) continue;

    std::string prefix = raw_prefix;
    for (auto &c : prefix) {
      if (c == '\\') c = '/';
    }
    // Strip a leading "./" — common artifact of cwd propagation.
    if (tinyusdz::startsWith(prefix, "./")) {
      prefix = tinyusdz::removePrefix(prefix, "./");
    }
    // Drop trailing slashes so we get exactly one separator.
    while (!prefix.empty() && prefix.back() == '/') {
      prefix.pop_back();
    }
    if (prefix.empty()) continue;

    const std::string candidate = prefix + "/" + asset_path;
    if (passet->asset_map.count(candidate)) {
      DCOUT("Resolved asset: " << asset_name << " as " << candidate);
      (*resolved_asset_name) = candidate;
      return 0;
    }

    // Normalize `./` and `../` segments and retry. This catches the deep-layer
    // relative-ref pattern described above.
    std::string normalized;
    if (NormalizeUSDZPath(candidate, &normalized) &&
        passet->asset_map.count(normalized)) {
      DCOUT("Resolved asset (normalized): " << asset_name << " as " << normalized);
      (*resolved_asset_name) = normalized;
      return 0;
    }
  }

  // 3) Suffix fallback. Real Unreal scenes have textures referenced from
  //    deeply-nested material files using paths like `Textures/T_Foo.png`
  //    that are relative to the *material's* layer directory. The composition
  //    resolver's `search_paths` reflects the root layer's cwd, not the
  //    per-material cwd, so the prefix-based lookup above misses those.
  //
  //    As a last resort, scan the asset map for entries whose full archive
  //    path ends with `/<asset_path>` and return one if exactly one matches.
  //    Unreal flattens texture filenames with a long unique prefix (e.g.
  //    `Game_AtmosphericHouse_Materials_Worn_MI_Foo_BaseColor.png`), so
  //    collisions are extremely rare in practice. If multiple matches exist
  //    we bail to avoid binding the wrong texture silently.
  const std::string suffix_match = "/" + asset_path;
  const std::string *unique_hit = nullptr;
  size_t hit_count = 0;
  for (const auto &kv : passet->asset_map) {
    const std::string &key = kv.first;
    if (key.size() <= suffix_match.size()) continue;
    if (key.compare(key.size() - suffix_match.size(),
                    suffix_match.size(), suffix_match) == 0) {
      unique_hit = &key;
      if (++hit_count > 1) break;
    }
  }
  if (hit_count == 1 && unique_hit) {
    DCOUT("Resolved asset (suffix-fallback): " << asset_name << " as " << *unique_hit);
    (*resolved_asset_name) = *unique_hit;
    return 0;
  }

  // 4) Escape-tolerant suffix fallback. Unreal exports sometimes author
  //    references using paths relative to the source project on the
  //    exporter's filesystem, not the USDZ archive root — e.g.
  //    `@../../../../../USD_Exports/AbandonedFactory/Assets/SM_Barrel.usd@`.
  //    Five levels of `..` mean NormalizeUSDZPath bails (root escape) and
  //    the literal suffix in step 3 starts with `/..` which never matches.
  //    Walk the path right-to-left, suffix-matching progressively shorter
  //    tails (`.../Assets/SM_Barrel.usd` → `Assets/SM_Barrel.usd` → just the
  //    basename), and return the first uniquely-matching tail. Last-resort
  //    only — the earlier strategies handle correctly-authored references.
  {
    // Trim leading "./" and "../" segments off asset_path; we only want the
    // forward-going tail.
    std::string tail = asset_path;
    while (true) {
      if (tinyusdz::startsWith(tail, "./")) {
        tail = tinyusdz::removePrefix(tail, "./");
      } else if (tinyusdz::startsWith(tail, "../")) {
        tail = tinyusdz::removePrefix(tail, "../");
      } else {
        break;
      }
    }

    // Walk component boundaries left-to-right inside `tail`, trying each
    // progressively shorter tail starting at component boundaries.
    size_t pos = 0;
    while (pos < tail.size()) {
      const std::string candidate_tail = tail.substr(pos);
      const std::string suffix = "/" + candidate_tail;
      const std::string *hit = nullptr;
      size_t n = 0;
      for (const auto &kv : passet->asset_map) {
        const std::string &key = kv.first;
        if (key == candidate_tail) {
          // Exact archive-root match.
          DCOUT("Resolved asset (escape-tolerant tail): " << asset_name
                                                          << " as " << key);
          (*resolved_asset_name) = key;
          return 0;
        }
        if (key.size() > suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) ==
                0) {
          hit = &key;
          if (++n > 1) break;
        }
      }
      if (n == 1 && hit) {
        DCOUT("Resolved asset (escape-tolerant tail): " << asset_name
                                                        << " as " << *hit);
        (*resolved_asset_name) = *hit;
        return 0;
      }
      // Advance to the next `/` boundary and try a shorter tail.
      size_t slash = tail.find('/', pos);
      if (slash == std::string::npos) break;
      pos = slash + 1;
    }
  }

  return -1; // not found
}

int USDZSizeAsset(const char *resolved_asset_name, uint64_t *nbytes, std::string *err, void *userdata) {

  if (!userdata) {
    if (err) {
      (*err) += "`userdata` must be non-null.\n";
    }
    return -2;
  }

  if (!resolved_asset_name) {
    if (err) {
      (*err) += "`resolved_asset_name` must be non-null.\n";
    }
    return -2;
  }

  if (!nbytes) {
    if (err) {
      (*err) += "`nbytes` must be non-null.\n";
    }
    return -2;
  }

  const USDZAsset *passet = reinterpret_cast<const USDZAsset *>(userdata);

  if (!passet->asset_map.count(resolved_asset_name)) {
    if (err) {
      (*err) += "resolved_asset_name `" + std::string(resolved_asset_name) + "` not found in USDZAsset.\n";
    }
    return -1;
  }

  std::pair<size_t, size_t> byte_range = passet->asset_map.at(resolved_asset_name);

  if (byte_range.first >= byte_range.second) {
    if (err) {
      (*err) += "Invalid USDZAsset byte range.\n";
    }
    return -2;
  }

  (*nbytes) = byte_range.second - byte_range.first;

  return 0;
}

int USDZReadAsset(const char *resolved_asset_name, uint64_t req_bytes, uint8_t *out_buf, uint64_t *nbytes, std::string *err, void *userdata) {
  if (!userdata) {
    if (err) {
      (*err) += "`userdata` must be non-null.\n";
    }
    return -1;
  }

  if (!resolved_asset_name) {
    if (err) {
      (*err) += "`resolved_asset_name` must be non-null.\n";
    }
    return -2;
  }

  if (!out_buf) {
    if (err) {
      (*err) += "`out_buf` must be non-null.\n";
    }
    return -2;
  }

  if (!nbytes) {
    if (err) {
      (*err) += "`nbytes` must be non-null.\n";
    }
    return -2;
  }

  const USDZAsset *passet = reinterpret_cast<const USDZAsset *>(userdata);

  if (!passet->asset_map.count(resolved_asset_name)) {
    if (err) {
      (*err) += "resolved_asset_name `" + std::string(resolved_asset_name) + "` not found in USDZAsset.\n";
    }
    return -1;
  }

  std::pair<size_t, size_t> byte_range = passet->asset_map.at(resolved_asset_name);

  if (byte_range.first >= byte_range.second) {
    if (err) {
      (*err) += "Invalid USDZAsset byte range.\n";
    }
    return -2;
  }

  size_t sz = byte_range.second - byte_range.first;

  if (sz > req_bytes) {
    if (err) {
      (*err) += "USDZAsset " + std::string(resolved_asset_name) + "'s size exceeds requested bytes.\n";
    }
    return -2;
  }

  if (byte_range.first + sz > passet->data.size()) {
    if (err) {
      (*err) += "Invalid USDZAsset size: " + std::string(resolved_asset_name) + "\n";
    }
    return -2;
  }

  memcpy(out_buf, passet->data.data() + byte_range.first, sz);
  (*nbytes) = sz;

  return 0;
}

bool SetupUSDZAssetResolution(
  AssetResolutionResolver &resolver,
  const USDZAsset *pusdzAsset)
{
  // https://openusd.org/release/spec_usdz.html
  //
  // TODO(LTE):
  //
  // [ ] USD: usda, usdc, usd
  // [ ] Audio: m4a, mp3, wav

  if (!pusdzAsset) {
    return false;
  }
  // TODO: Validate Asset data.

  AssetResolutionHandler handler;
  handler.resolve_fun = USDZResolveAsset;
  handler.size_fun = USDZSizeAsset;
  handler.read_fun = USDZReadAsset;
  handler.write_fun = nullptr;
  handler.userdata = reinterpret_cast<void *>(const_cast<USDZAsset *>(pusdzAsset));

  // USD layers — needed so composition (`prepend references = @./*.usdc@`)
  // can resolve sibling USD files inside the USDZ archive. The original code
  // only registered image extensions and left this as a TODO above; without
  // it, references silently get dropped during CompositeReferences (the
  // resolver returns empty → composition warns + treats it as success).
  resolver.register_asset_resolution_handler("usd", handler);
  resolver.register_asset_resolution_handler("USD", handler);
  resolver.register_asset_resolution_handler("usda", handler);
  resolver.register_asset_resolution_handler("USDA", handler);
  resolver.register_asset_resolution_handler("usdc", handler);
  resolver.register_asset_resolution_handler("USDC", handler);

  resolver.register_asset_resolution_handler("png", handler);
  resolver.register_asset_resolution_handler("PNG", handler);
  resolver.register_asset_resolution_handler("JPG", handler);
  resolver.register_asset_resolution_handler("jpg", handler);
  resolver.register_asset_resolution_handler("jpeg", handler);
  resolver.register_asset_resolution_handler("JPEG", handler);
  resolver.register_asset_resolution_handler("exr", handler);
  resolver.register_asset_resolution_handler("EXR", handler);
  // HDR (Radiance HDR format) - commonly used for environment maps
  resolver.register_asset_resolution_handler("hdr", handler);
  resolver.register_asset_resolution_handler("HDR", handler);

  return true;
}

}  // namespace tinyusdz
