import re

with open('src/logic/GeometryValidity.cpp', 'r') as f:
    content = f.read()

# We want to remove the anonymous namespace entirely.
# It starts with "namespace\n{" after "namespace GeometryValidity\n{"
# and ends with "} // namespace" before "OpenBoundaryDebugStats CollectOpenBoundaryDebugStatsForSolid"

pattern = re.compile(r'namespace\s*\{\s*\[\[nodiscard\]\].*?\} // namespace\s*', re.DOTALL)
new_content = pattern.sub('', content)

with open('src/logic/GeometryValidity.cpp', 'w') as f:
    f.write(new_content)
