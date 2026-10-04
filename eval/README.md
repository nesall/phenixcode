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


