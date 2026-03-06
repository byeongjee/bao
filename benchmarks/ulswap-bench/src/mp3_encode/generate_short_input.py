#!/usr/bin/env python3
"""Generate a shorter input.h for MSP430 compatibility.

The original 19.64 second audio causes flash overflow on MSP430FR5994.
This script creates a 2-second version that fits in flash.
"""

import re
import struct

def read_input_h(filepath):
    """Read input.h and extract raw bytes."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Extract hex values from the array
    hex_pattern = r'0x([0-9A-Fa-f]{2})'
    matches = re.findall(hex_pattern, content)
    return bytes(int(h, 16) for h in matches)

def create_truncated_wav(data, target_seconds=2.0):
    """Truncate WAV data to target duration, updating headers."""
    # Parse WAV header
    # Bytes 24-27: sample rate (little endian)
    sample_rate = struct.unpack('<I', data[24:28])[0]
    # Bytes 34-35: bits per sample
    bits_per_sample = struct.unpack('<H', data[34:36])[0]
    # Bytes 22-23: channels
    channels = struct.unpack('<H', data[22:24])[0]

    bytes_per_sample = (bits_per_sample // 8) * channels
    target_samples = int(target_seconds * sample_rate)
    target_data_bytes = target_samples * bytes_per_sample

    # WAV header is 44 bytes, data starts at byte 44
    header = bytearray(data[:44])
    audio_data = data[44:44 + target_data_bytes]

    # Update RIFF chunk size (bytes 4-7): file size - 8
    new_file_size = 44 + len(audio_data)
    struct.pack_into('<I', header, 4, new_file_size - 8)

    # Update data chunk size (bytes 40-43)
    struct.pack_into('<I', header, 40, len(audio_data))

    return bytes(header) + audio_data

def write_input_h(filepath, data):
    """Write data as C header file."""
    with open(filepath, 'w') as f:
        f.write('#ifndef INPUT_H\n')
        f.write('#define INPUT_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('const uint8_t test_data[] =\n{\n')

        for i in range(0, len(data), 8):
            chunk = data[i:i+8]
            hex_str = ', '.join(f'0x{b:02X}' for b in chunk)
            if i + 8 < len(data):
                f.write(f'    {hex_str},\n')
            else:
                f.write(f'    {hex_str}\n')

        f.write('};\n\n')
        f.write('#endif // INPUT_H\n')

def main():
    import sys

    input_file = 'input.h'
    output_file = 'input_short.h'
    target_seconds = 2.0

    if len(sys.argv) > 1:
        target_seconds = float(sys.argv[1])

    print(f"Reading {input_file}...")
    data = read_input_h(input_file)
    print(f"Original size: {len(data)} bytes")

    # Parse original WAV info
    sample_rate = struct.unpack('<I', data[24:28])[0]
    bits_per_sample = struct.unpack('<H', data[34:36])[0]
    channels = struct.unpack('<H', data[22:24])[0]
    original_data_size = struct.unpack('<I', data[40:44])[0]
    original_duration = original_data_size / (sample_rate * channels * bits_per_sample // 8)

    print(f"Sample rate: {sample_rate} Hz")
    print(f"Bits per sample: {bits_per_sample}")
    print(f"Channels: {channels}")
    print(f"Original duration: {original_duration:.2f} seconds")

    print(f"\nTruncating to {target_seconds} seconds...")
    truncated = create_truncated_wav(data, target_seconds)
    print(f"New size: {len(truncated)} bytes")

    new_duration = (len(truncated) - 44) / (sample_rate * channels * bits_per_sample // 8)
    print(f"New duration: {new_duration:.2f} seconds")

    print(f"\nWriting {output_file}...")
    write_input_h(output_file, truncated)
    print("Done!")

    # Estimate binary size reduction
    savings = len(data) - len(truncated)
    print(f"\nEstimated binary size reduction: {savings:,} bytes ({savings/1024:.1f} KB)")

if __name__ == '__main__':
    main()
