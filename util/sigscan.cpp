#include "sigscan.h"

#include <sstream>
#include <vector>

#include "util/logging.h"
#include "util/memutils.h"
#include "util/utils.h"

intptr_t find_pattern(std::vector<uint8_t> &data, intptr_t base, const uint8_t *pattern,
        const char *mask, intptr_t offset, intptr_t usage)
{

    // build pattern
    std::vector<std::pair<uint8_t, bool>> pattern_vector;
    size_t mask_size = strlen(mask);
    for (size_t i = 0; i < mask_size; i++) {
        pattern_vector.emplace_back(pattern[i], mask[i] == 'X');
    }

    // the scan loop
    auto data_begin = data.begin();
    auto cur_usage = 0;
    while (true) {

        // search for the pattern
        auto search_result = std::search(data_begin, data.end(), pattern_vector.begin(), pattern_vector.end(),
                               [&](uint8_t c, std::pair<uint8_t, bool> pat) {
                                   return (!pat.second) || c == pat.first;
                               });

        // check for a match
        if (search_result != data.end()) {

            // return the result if we hit the usage count
            if (cur_usage == usage) {
                return (std::distance(data.begin(), search_result) + base) + offset;
            }

            // increment the found count
            ++cur_usage;
            data_begin = ++search_result;
        } else {
            break;
        }
    }

    return 0;
}

intptr_t find_pattern(HMODULE module, const uint8_t *pattern, const char *mask,
        intptr_t offset, intptr_t result_usage)
{
    // get module information
    MODULEINFO module_info {};
    if (!GetModuleInformation(GetCurrentProcess(), module, &module_info, sizeof(module_info))) {
        return 0;
    }

    auto size = static_cast<size_t>(module_info.SizeOfImage);

    try {

        // copy data
        std::vector<uint8_t> data(size);
        memcpy(data.data(), module_info.lpBaseOfDll, size);

        // find pattern
        return find_pattern(
                data,
                reinterpret_cast<intptr_t>(module_info.lpBaseOfDll),
                pattern,
                mask,
                offset,
                result_usage);
    } catch (const std::bad_alloc &e) {
        log_warning("sigscan", "failed to allocate buffer of size {} for image data", size);
        return false;
    }
}

intptr_t replace_pattern(HMODULE module, const uint8_t *pattern, const char *mask, intptr_t offset,
        intptr_t usage, const uint8_t *replace_data, const char *replace_mask)
{

    // find result
    auto result = find_pattern(module, pattern, mask, offset, usage);

    // check result
    if (!result) {
        return 0;
    }

    // unprotect memory
    auto replace_mask_len = strlen(replace_mask);
    memutils::VProtectGuard guard((void *) result, replace_mask_len);

    // replace data
    for (size_t i = 0; i < replace_mask_len; i++) {
        if (replace_mask[i] == 'X') {
            *((unsigned char *) (result + i)) = replace_data[i];
        }
    }

    // success
    return result;
}

intptr_t replace_pattern(HMODULE module, const std::string &signature,
        const std::string &replacement, intptr_t offset, intptr_t usage)
{
    // build pattern
    std::string pattern_str(signature);
    strreplace(pattern_str, "??", "00");
    auto pattern_bin = std::make_unique<uint8_t[]>(signature.length() / 2);
    if (!hex2bin(pattern_str.c_str(), pattern_bin.get())) {
        return false;
    }

    // build signature mask
    std::ostringstream signature_mask;
    for (size_t i = 0; i < signature.length(); i += 2) {
        if (signature[i] == '?') {
            if (signature[i + 1] == '?') {
                signature_mask << '?';
            } else {
                return false;
            }
        } else {
            signature_mask << 'X';
        }
    }

    // build replace data
    std::string replace_data_str(replacement);
    strreplace(replace_data_str, "??", "00");
    auto replace_data_bin = std::make_unique<uint8_t[]>(replacement.length() / 2);
    if (!hex2bin(replace_data_str.c_str(), replace_data_bin.get())) {
        return false;
    }

    // build replace mask
    std::ostringstream replace_mask;
    for (size_t i = 0; i < replacement.length(); i += 2) {
        if (replacement[i] == '?') {
            if (replacement[i + 1] == '?') {
                replace_mask << '?';
            } else {
                return false;
            }
        } else {
            replace_mask << 'X';
        }
    }

    // do the replacement
    return replace_pattern(
            module,
            pattern_bin.get(),
            signature_mask.str().c_str(),
            offset,
            usage,
            replace_data_bin.get(),
            replace_mask.str().c_str()
    );
}
