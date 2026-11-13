# The date of the release entry in the AppStream metainfo, which Discover, GNOME Software and other
# AppStream readers show as the date of this version.
#
# A date-style version, YYYY.MDD.N or YYYY.MMDD.N (the old upstream scheme), carries its own date.
# Polaris versions are semver and carry none, and until #673 every one of them, 1.5.0 included, was
# dated 1990-01-01. For those the date comes from the first of:
#
#   1. POLARIS_RELEASE_DATE=YYYY-MM-DD, as a CMake variable or else from the environment. A value
#      that is not a real calendar date in that form fails the configure.
#   2. The committer date of HEAD, in UTC, when the source directory is a git checkout. git's own
#      %cs prints that date in the committer's time zone, which can be a day away from UTC.
#   3. SOURCE_DATE_EPOCH, only when it is later than 2020-01-01. Nix's stdenv sets it to
#      1980-01-01, which is no more a release date than 1990-01-01 is.
#   4. 1990-01-01, with a warning.
#
# src_assets/common/assets/web/release-date-contract.test.js pins each of these through the real
# metainfo template.
#
# polaris_release_date(<version> <source_dir>) sets, in the caller's scope:
#   PROJECT_YEAR, PROJECT_MONTH, PROJECT_DAY  the date, zero-padded
#   POLARIS_RELEASE_DATE_SOURCE               where it came from

# A SOURCE_DATE_EPOCH at or before 2020-01-01T00:00:00Z is a placeholder, like Nix's 1980-01-01,
# rather than the time of a Polaris release.
set(POLARIS_RELEASE_DATE_EPOCH_FLOOR 1577836800)

# Format a Unix time as a UTC date. string(TIMESTAMP) reads its time from SOURCE_DATE_EPOCH when that
# is set, which is the one way CMake has to format a given time, so this sets it for the one call
# and then puts back whatever was there.
function(_polaris_release_date_from_epoch epoch out_var)
    set(had_epoch FALSE)
    if(DEFINED ENV{SOURCE_DATE_EPOCH})
        set(had_epoch TRUE)
        set(saved_epoch "$ENV{SOURCE_DATE_EPOCH}")
    endif()
    set(ENV{SOURCE_DATE_EPOCH} "${epoch}")
    string(TIMESTAMP date "%Y-%m-%d" UTC)
    if(had_epoch)
        set(ENV{SOURCE_DATE_EPOCH} "${saved_epoch}")
    else()
        unset(ENV{SOURCE_DATE_EPOCH})
    endif()
    set(${out_var} "${date}" PARENT_SCOPE)
endfunction()

# Fail the configure unless value is YYYY-MM-DD and names a day that exists.
function(_polaris_release_date_validate value)
    if(NOT value MATCHES "^([0-9][0-9][0-9][0-9])-([0-9][0-9])-([0-9][0-9])$")
        message(FATAL_ERROR "POLARIS_RELEASE_DATE must be a date as YYYY-MM-DD, not '${value}'")
    endif()
    set(month "${CMAKE_MATCH_2}")
    set(day "${CMAKE_MATCH_3}")
    # The year goes through math() for the leap-year test, so it loses its leading zeros first.
    string(REGEX REPLACE "^0+" "" year "${CMAKE_MATCH_1}")
    set(days_in_month 31)
    if(month EQUAL 4 OR month EQUAL 6 OR month EQUAL 9 OR month EQUAL 11)
        set(days_in_month 30)
    elseif(month EQUAL 2 AND NOT year STREQUAL "")
        math(EXPR by4 "${year} % 4")
        math(EXPR by100 "${year} % 100")
        math(EXPR by400 "${year} % 400")
        if(by400 EQUAL 0 OR (by4 EQUAL 0 AND NOT by100 EQUAL 0))
            set(days_in_month 29)
        else()
            set(days_in_month 28)
        endif()
    endif()
    if(year STREQUAL "" OR month LESS 1 OR month GREATER 12 OR day LESS 1 OR day GREATER days_in_month)
        message(FATAL_ERROR "POLARIS_RELEASE_DATE '${value}' is not a calendar date")
    endif()
endfunction()

function(polaris_release_date version source_dir)
    # Read the override first, so a malformed one fails the configure whatever the version is.
    set(override "")
    if(DEFINED POLARIS_RELEASE_DATE AND NOT POLARIS_RELEASE_DATE STREQUAL "")
        set(override "${POLARIS_RELEASE_DATE}")
    elseif(DEFINED ENV{POLARIS_RELEASE_DATE} AND NOT "$ENV{POLARIS_RELEASE_DATE}" STREQUAL "")
        set(override "$ENV{POLARIS_RELEASE_DATE}")
    endif()
    if(NOT override STREQUAL "")
        _polaris_release_date_validate("${override}")
    endif()

    set(year "1990")
    set(month "01")
    set(day "01")
    set(source "")

    # Note: Cmake doesn't support "{}" regex syntax
    if(version MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9][0-9][0-9][0-9]?)\\.([0-9]+)$")
        message("Extracting year and month/day from PROJECT_VERSION: ${version}")
        # First capture group is the year
        set(year "${CMAKE_MATCH_1}")

        # Second capture group contains month and day
        set(month_day "${CMAKE_MATCH_2}")

        # Extract month (first 1-2 digits) and day (last 2 digits)
        string(LENGTH "${month_day}" month_day_length)
        if(month_day_length EQUAL 3)
            # Format: MDD (e.g., 703 = month 7, day 03)
            string(SUBSTRING "${month_day}" 0 1 month)
            string(SUBSTRING "${month_day}" 1 2 day)
        elseif(month_day_length EQUAL 4)
            # Format: MMDD (e.g., 1203 = month 12, day 03)
            string(SUBSTRING "${month_day}" 0 2 month)
            string(SUBSTRING "${month_day}" 2 2 day)
        endif()

        # Ensure month is two digits
        if(month LESS 10 AND NOT month MATCHES "^0")
            set(month "0${month}")
        endif()
        # Ensure day is two digits
        if(day LESS 10 AND NOT day MATCHES "^0")
            set(day "0${day}")
        endif()
        set(source "date-style version")
        if(NOT override STREQUAL "")
            message(STATUS "POLARIS_RELEASE_DATE is not used: date-style version ${version} carries its own date")
        endif()
    else()
        set(date "")
        if(NOT override STREQUAL "")
            set(date "${override}")
            set(source "POLARIS_RELEASE_DATE")
        endif()

        if(date STREQUAL "" AND EXISTS "${source_dir}/.git")
            if(NOT GIT_EXECUTABLE)
                find_package(Git QUIET)
            endif()
            if(GIT_EXECUTABLE)
                execute_process(
                        COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${source_dir}" log -1 --format=%ct HEAD
                        WORKING_DIRECTORY "${source_dir}"
                        OUTPUT_VARIABLE commit_epoch
                        RESULT_VARIABLE git_result
                        ERROR_QUIET
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(git_result EQUAL 0 AND commit_epoch MATCHES "^[0-9]+$")
                    _polaris_release_date_from_epoch("${commit_epoch}" date)
                    set(source "HEAD commit date")
                endif()
            endif()
        endif()

        set(ignored_epoch "")
        if(date STREQUAL "" AND DEFINED ENV{SOURCE_DATE_EPOCH})
            set(epoch "$ENV{SOURCE_DATE_EPOCH}")
            if(epoch MATCHES "^[0-9]+$" AND epoch GREATER POLARIS_RELEASE_DATE_EPOCH_FLOOR)
                _polaris_release_date_from_epoch("${epoch}" date)
                set(source "SOURCE_DATE_EPOCH")
            else()
                set(ignored_epoch " SOURCE_DATE_EPOCH=${epoch} was ignored: it is not a time after 2020-01-01.")
            endif()
        endif()

        if(date STREQUAL "")
            message(WARNING
                    "No release date for Polaris ${version}, so the AppStream release entry says 1990-01-01. "
                    "Build from a git checkout, or set POLARIS_RELEASE_DATE=YYYY-MM-DD.${ignored_epoch}")
            set(source "fallback")
        else()
            string(SUBSTRING "${date}" 0 4 year)
            string(SUBSTRING "${date}" 5 2 month)
            string(SUBSTRING "${date}" 8 2 day)
        endif()
    endif()

    set(PROJECT_YEAR "${year}" PARENT_SCOPE)
    set(PROJECT_MONTH "${month}" PARENT_SCOPE)
    set(PROJECT_DAY "${day}" PARENT_SCOPE)
    set(POLARIS_RELEASE_DATE_SOURCE "${source}" PARENT_SCOPE)
endfunction()
