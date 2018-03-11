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
check_component "inxi" "-CGASMzx -! 31"
check_component "vainfo" ""
check_component "ffmpeg" "-version" | sed '/^configuration:/d'
check_component "gcc" "-dumpversion"
check_component "svdrpsend" "plug vaapidevice dbug" | sed 's/^\(22[0-1]\) [a-zA-Z0-9]* \(.*\)$/\1 <filter> \2/'
echo "##### INCLUDE THIS REPORT INTO YOUR GITHUB ISSUE"
