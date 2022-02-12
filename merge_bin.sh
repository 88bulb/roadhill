#!/bin/bash

# 0x1000 bootloader/bootloader.bin 
# 0x10000 roadhill.bin 
# 0x8000 partition_table/partition-table.bin 
# 0xd000 ota_data_initial.bin
esptool.py \
    --chip esp32 merge_bin \
    -o roadhill_merged.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x01000 build/bootloader/bootloader.bin \
    0x08000 build/partition_table/partition-table.bin \
    0x0d000 build/ota_data_initial.bin \
    0x10000 build/roadhill.bin 

