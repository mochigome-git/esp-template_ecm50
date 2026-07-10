cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS w5500) 
set(PARTITION_TABLE_CSV_PATH "partitions.csv") 
# Pull in ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp-mpm001pl-ecm50)
