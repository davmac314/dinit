#!/usr/bin/env sh

## Gen man-pages in meson builds

# Why dinit dont use "env" argument in "run_command()"?
# Because old meson (>0.50.0) dont support "env" in "run_command()".
# Meson warning:
# WARNING: Project specifies a minimum meson_version '>= 0.47.0' but uses features which were added in newer versions:
#  * 0.50.0: {'env arg in run_command'}
# We use command argument instead of that.
SOURCE_MAN_PATH=$1
TARGET_MAN_PATH=$2
VERSION=$3
MONTH=$4
YEAR=$5

# Generate man-pages
m4 -DVERSION=$VERSION -DMONTH=$MONTH -DYEAR=$YEAR $SOURCE_MAN_PATH/dinit.8.m4 > $TARGET_MAN_PATH/dinit.8
m4 -DVERSION=$VERSION -DMONTH=$MONTH -DYEAR=$YEAR $SOURCE_MAN_PATH/dinitctl.8.m4 > $TARGET_MAN_PATH/dinitctl.8
m4 -DVERSION=$VERSION -DMONTH=$MONTH -DYEAR=$YEAR $SOURCE_MAN_PATH/dinitcheck.8.m4 > $TARGET_MAN_PATH/dinitcheck.8
m4 -DVERSION=$VERSION -DMONTH=$MONTH -DYEAR=$YEAR $SOURCE_MAN_PATH/dinit-monitor.8.m4 > $TARGET_MAN_PATH/dinit-monitor.8
m4 -DVERSION=$VERSION -DMONTH=$MONTH -DYEAR=$YEAR $SOURCE_MAN_PATH/dinit-service.5.m4 > $TARGET_MAN_PATH/dinit-service.5
m4 -DVERSION=$VERSION -DMONTH=$MONTH -DYEAR=$YEAR $SOURCE_MAN_PATH/shutdown.8.m4 > $TARGET_MAN_PATH/shutdown.8
