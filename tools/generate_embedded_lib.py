#!/usr/bin/env python3
"""
Tool to convert a shared library file to C++ byte array for embedding
"""

import sys
import os

def generate_embedded_lib(so_file_path, output_header_path):
    """Convert .so file to C++ byte array header"""
    
    if not os.path.exists(so_file_path):
        print(f"Error: {so_file_path} does not exist")
        return False
    
    with open(so_file_path, 'rb') as f:
        data = f.read()
    
    header_content = f'''#ifndef INTERCEPTOR_EMBEDDED_H
#define INTERCEPTOR_EMBEDDED_H

#include <cstddef>

namespace EmbeddedInterceptor {{

// Size of the embedded interceptor library
const size_t INTERCEPTOR_SIZE = {len(data)};

// Embedded interceptor library data
const unsigned char INTERCEPTOR_DATA[] = {{
'''
    
    # Convert bytes to C++ array format
    bytes_per_line = 12
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i:i+bytes_per_line]
        hex_values = [f"0x{b:02x}" for b in chunk]
        header_content += "    " + ", ".join(hex_values)
        if i + bytes_per_line < len(data):
            header_content += ","
        header_content += "\n"
    
    header_content += '''};

} // namespace EmbeddedInterceptor

#endif // INTERCEPTOR_EMBEDDED_H
'''
    
    # Write header file
    with open(output_header_path, 'w') as f:
        f.write(header_content)
    
    print(f"Generated embedded library header: {output_header_path}")
    print(f"Original library size: {len(data)} bytes")
    return True

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: generate_embedded_lib.py <input.so> <output.h>")
        sys.exit(1)
    
    so_file = sys.argv[1]
    header_file = sys.argv[2]
    
    if generate_embedded_lib(so_file, header_file):
        sys.exit(0)
    else:
        sys.exit(1)