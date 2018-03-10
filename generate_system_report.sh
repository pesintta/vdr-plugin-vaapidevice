#!/bin/sh

check_component () {
    BIN=$(command -v "${1}" 2>/dev/null)
    echo "##### ${1}"
    echo "\`\`\`"
    if [ -n "${BIN}" ]; then
        ${BIN} "${2}" 2>/dev/null
    else
        echo "The '${1}' command not found! Please, install!"
    fi
    echo "\`\`\`"
}

echo "#### VAAPIDEVICE SYSTEM INFORMATION REPORT"
check_component "inxi" "-CGASMNzx -! 31"
check_component "vainfo" ""
check_component "ffmpeg" "-version"
echo "##### INCLUDE THIS REPORT INTO YOUR GITHUB ISSUE"
