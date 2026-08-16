#!/bin/bash
# Script to open HTML version of README files
# Usage: ./scripts/open-readme.sh [lang]
# Options: zh (Chinese, default), en (English), all (both)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LANG="${1:-zh}"

case "$LANG" in
    "zh"|"chinese"|"zh-cn"|"")
        echo "Opening Chinese HTML README..."
        if command -v open >/dev/null 2>&1; then
            open "${PROJECT_ROOT}/README.html"
        elif command -v xdg-open >/dev/null 2>&1; then
            xdg-open "${PROJECT_ROOT}/README.html"
        else
            echo "Please open ${PROJECT_ROOT}/README.html in your browser"
        fi
        ;;
    "en"|"english")
        echo "Opening English HTML README..."
        if command -v open >/dev/null 2>&1; then
            open "${PROJECT_ROOT}/README.en.html"
        elif command -v xdg-open >/dev/null 2>&1; then
            xdg-open "${PROJECT_ROOT}/README.en.html"
        else
            echo "Please open ${PROJECT_ROOT}/README.en.html in your browser"
        fi
        ;;
    "all")
        echo "Opening both HTML README files..."
        if command -v open >/dev/null 2>&1; then
            open "${PROJECT_ROOT}/README.html"
            open "${PROJECT_ROOT}/README.en.html"
        elif command -v xdg-open >/dev/null 2>&1; then
            xdg-open "${PROJECT_ROOT}/README.html"
            xdg-open "${PROJECT_ROOT}/README.en.html"
        else
            echo "Please open these files in your browser:"
            echo "  ${PROJECT_ROOT}/README.html"
            echo "  ${PROJECT_ROOT}/README.en.html"
        fi
        ;;
    "help"|"-h"|"--help")
        echo "Usage: $0 [lang]"
        echo ""
        echo "Options:"
        echo "  zh, chinese    Open Chinese HTML README (default)"
        echo "  en, english    Open English HTML README"
        echo "  all            Open both HTML README files"
        echo "  help           Show this help message"
        echo ""
        echo "Examples:"
        echo "  $0             # Open Chinese version (default)"
        echo "  $0 en          # Open English version"
        echo "  $0 all         # Open both versions"
        ;;
    *)
        echo "Error: Unknown language '$LANG'"
        echo "Use '$0 help' for usage information"
        exit 1
        ;;
esac
