find . \
  \( -path './build' -o -path './.git' -o -path './node_modules' -o -path './build_dbg' -o -path './build_rel' -o -path './include/3rdparty' -o -path './ui' \) -prune -o \
  -type f \( -name "*.cpp" -o -name "*.h" \) -print |
while read -r f; do
    rel="${PWD##*/}/${f#./}"
    echo -e "## File: ${rel}\n"
    cat "$f"
    echo -e "\n\n"
done > combined_src.txt