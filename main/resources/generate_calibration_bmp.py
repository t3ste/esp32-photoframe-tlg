#!/usr/bin/env python3
"""
Generate a calibration BMP with 6 color boxes for E6 display calibration.
The BMP uses the theoretical palette values that the firmware expects.
"""

import struct
import sys

# Display dimensions
WIDTH = 800
HEIGHT = 480

# Theoretical E6 palette (matches firmware)
PALETTE = [
    (0, 0, 0),        # 0: Black
    (255, 255, 255),  # 1: White
    (255, 255, 0),    # 2: Yellow
    (255, 0, 0),      # 3: Red
    (0, 0, 0),        # 4: Reserved (unused)
    (0, 0, 255),      # 5: Blue
    (0, 255, 0)       # 6: Green
]

# Color indices to use (skip reserved color 4)
COLOR_INDICES = [0, 1, 2, 3, 5, 6]  # Black, White, Yellow, Red, Blue, Green

def create_calibration_bmp(output_path):
    """Create a 24-bit RGB BMP with 6 color boxes arranged in 2 rows x 3 columns."""
    
    # Calculate box dimensions - no padding, boxes fill entire display
    box_width = WIDTH // 3
    box_height = HEIGHT // 2
    
    # Create image buffer (3 bytes per pixel for 24-bit RGB)
    # BMP stores pixels as BGR, and rows must be padded to 4-byte boundary
    row_size = WIDTH * 3
    padding_bytes = (4 - (row_size % 4)) % 4
    pixel_data_size = (row_size + padding_bytes) * HEIGHT
    
    # Create a temporary buffer for easier manipulation (top-down)
    temp_buffer = bytearray(WIDTH * HEIGHT * 3)
    
    # Draw 6 color boxes (2 rows x 3 columns) - no background fill needed
    for idx, color_idx in enumerate(COLOR_INDICES):
        row = idx // 3
        col = idx % 3
        
        x_start = col * box_width
        y_start = row * box_height
        
        # Handle remainder pixels for last column/row
        x_end = (col + 1) * box_width if col < 2 else WIDTH
        y_end = (row + 1) * box_height if row < 1 else HEIGHT
        
        r, g, b = PALETTE[color_idx]
        
        # Fill the box with the color
        for y in range(y_start, y_end):
            for x in range(x_start, x_end):
                pixel_offset = (y * WIDTH + x) * 3
                temp_buffer[pixel_offset] = b      # B
                temp_buffer[pixel_offset + 1] = g  # G
                temp_buffer[pixel_offset + 2] = r  # R
    
    # Convert to bottom-up BMP format with padding
    pixels = bytearray(pixel_data_size)
    for y in range(HEIGHT):
        # BMP rows are stored bottom-to-top
        bmp_row = HEIGHT - 1 - y
        src_offset = y * WIDTH * 3
        dst_offset = bmp_row * (row_size + padding_bytes)
        
        # Copy row data
        for x in range(WIDTH):
            pixels[dst_offset + x * 3] = temp_buffer[src_offset + x * 3]
            pixels[dst_offset + x * 3 + 1] = temp_buffer[src_offset + x * 3 + 1]
            pixels[dst_offset + x * 3 + 2] = temp_buffer[src_offset + x * 3 + 2]
        
        # Padding bytes are already zero-initialized
    
    # Write BMP file
    with open(output_path, 'wb') as f:
        # BMP Header (14 bytes)
        f.write(b'BM')  # Signature
        
        # Calculate file size (no palette for 24-bit)
        file_size = 14 + 40 + pixel_data_size
        
        f.write(struct.pack('<I', file_size))  # File size
        f.write(struct.pack('<HH', 0, 0))      # Reserved
        f.write(struct.pack('<I', 14 + 40))    # Pixel data offset (no palette)
        
        # DIB Header (BITMAPINFOHEADER - 40 bytes)
        f.write(struct.pack('<I', 40))         # Header size
        f.write(struct.pack('<i', WIDTH))      # Width
        f.write(struct.pack('<i', HEIGHT))     # Height (positive = bottom-up)
        f.write(struct.pack('<H', 1))          # Planes
        f.write(struct.pack('<H', 24))         # Bits per pixel (24-bit RGB)
        f.write(struct.pack('<I', 0))          # Compression (none)
        f.write(struct.pack('<I', pixel_data_size))  # Image size
        f.write(struct.pack('<i', 2835))       # X pixels per meter
        f.write(struct.pack('<i', 2835))       # Y pixels per meter
        f.write(struct.pack('<I', 0))          # Colors used (0 = all colors)
        f.write(struct.pack('<I', 0))          # Important colors (0 = all)
        
        # Pixel data (no palette for 24-bit)
        f.write(pixels)
    
    print(f"Calibration BMP created: {output_path}")
    print(f"Image size: {WIDTH}x{HEIGHT}")
    print(f"Box arrangement: 2 rows x 3 columns")
    print(f"Colors: Black, White, Yellow, Red, Blue, Green")

if __name__ == '__main__':
    output_file = sys.argv[1] if len(sys.argv) > 1 else 'calibration.bmp'
    create_calibration_bmp(output_file)
