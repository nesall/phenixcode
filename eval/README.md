```
cd eval
```

# 1. Generate template (samples N files, creates queries)
```
../build_dbg/Debug/phenixcode-core.exe -c D:/workspace/misc/phenixcode_projects/settings_phenixcode.json eval --init --samples 30
```

# 2. Discover available doc IDs
```
../build_dbg/Debug/phenixcode-core.exe -c D:/workspace/misc/phenixcode_projects/settings_phenixcode.json eval --list 
```

# 3. Inspect a specific doc
```
../build_dbg/Debug/phenixcode-core.exe -c D:/workspace/misc/phenixcode_projects/settings_phenixcode.json eval --detail D:/wor
```

# 4. Edit eval.json to correct expected_doc_ids

# 5. Run evaluation
```
../build_dbg/Debug/phenixcode-core.exe -w D:/workspace/misc/phenixcode_projects/providers.json -c D:/workspace/misc/phenixcode_projects/settings_phenixcode.json eval --dataset eval.json --top 5
```

# 7 Insert vectors into the question file (for determinism)
```
cd /d/workspace/misc/tmp/phenixcode
.../build_dbg/Debug/phenixcode-core.exe -w D:/workspace/misc/phenixcode_projects/providers.json -c /d/workspace/misc/tmp/phenixcode/settings.json eval --dataset /d/workspace/projects/phenixcode/eval/eval.json  --add-vectors --output eval-with-vecs.json
```

# 6 Sweep run
```
cd /d/workspace/misc/tmp/phenixcode
../build_dbg/Debug/phenixcode-core.exe -w D:/workspace/misc/phenixcode_projects/providers.json -c /d/workspace/misc/tmp/phenixcode/settings.json eval --dataset /d/workspace/misc/tmp/phenixcode/eval/eval-with-vecs.json --sweep --bm25-weight 0.5 --grep-weight 0.3 0.6 --top 5 8
```

