#include "base32.hpp"

#include <vector>

namespace base32 {

std::string encode_nopad(const std::string& input) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string output;
    size_t i = 0, index = 0, digit = 0;
    int curr_byte, next_byte;

    while (i < input.size()) {
        curr_byte = input[i] & 0xFF;
        if (index > 3) {
            if ((i + 1) < input.size())
                next_byte = input[i + 1] & 0xFF;
            else
                next_byte = 0;
            digit = curr_byte & (0xFF >> index);
            index = (index + 5) % 8;
            digit <<= index;
            digit |= next_byte >> (8 - index);
            i++;
        } else {
            digit = (curr_byte >> (8 - (index + 5))) & 0x1F;
            index = (index + 5) % 8;
            if (index == 0) i++;
        }
        output += alphabet[digit];
    }
    return output;
}

} // namespace base32
