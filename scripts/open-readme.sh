#!/bin/bash
# Script to open HTML version of README files
# Usage: ./scripts/open-readme.sh [lang]
# Options: en (English), zh (Chinese), all (both)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LANG="${1:-en}"

case "$LANG" in
    "en"|"english"|"")
        echo "Opening English HTML README..."
        if command -v open >/dev/null 2>&1; then
            open "${PROJECT_ROOT}/README.html"
        elif command -v xdg-open >/dev/null 2>&1; then
            xdg-open "${PROJECT_ROOT}/README.html"
        else
            echo "Please open ${PROJECT_ROOT}/README.html in your browser"
        fi
        ;;
    "zh"|"chinese"|"zh-cn")
        echo "Opening Chinese HTML README..."
        if command -v open >/dev/null 2>&1; then
            open "${PROJECT_ROOT}/README.zh-CN.html"
        elif command -v xdg-open >/dev/null 2>&1; then
            xdg-open "${PROJECT_ROOT}/README.zh-CN.html"
        else
            echo "Please open ${PROJECT_ROOT}/README.zh-CN.html in your browser"
        fi
        ;;
    "all")
        echo "Opening both HTML README files..."
        if command -v open >/dev/null 2>&1; then
            open "${PROJECT_ROOT}/README.html"
            open "${PROJECT_ROOT}/README.zh-CN.html"
        elif command -v xdg-open >/dev/null 2>&1; then
            xdg-open "${PROJECT_ROOT}/README.html"
            xdg-open "${PROJECT_ROOT}/README.zh-CN.html"
        else
            echo "Please open these files in your browser:"
            echo "  ${PROJECT_ROOT}/README.html"
            echo "  ${PROJECT_ROOT}/README.zh-CN.html"
        fi
        ;;
    "help"|"-h"|"--help")
        echo "Usage: $0 [lang]"
        echo ""
        echo "Options:"
        echo "  en, english    Open English HTML README (default)"
        echo "  zh, chinese   Open Chinese HTML README"
        echo "  all           Open both HTML README files"
        echo "  help          Show this help message"
        echo ""
        echo "Examples:"
        echo "  $0            # Open English version"
        echo "  $0 zh         # Open Chinese version"
        echo "  $0 all        # Open both versions"
        ;;
    *)
        echo "Error: Unknown language '$LANG'"
        echo "Use '$0 help' for usage information"
        exit 1
        ;;
esac