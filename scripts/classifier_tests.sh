#!/usr/bin/env bash

set -u

# ============================================================
# Classifier comparison: Mistral Small 4 vs GLM-5.3 Flash
# ============================================================

MISTRAL_MODEL="mistral-small-latest"
GLM_MODEL="glm-5.3-flash"

MISTRAL_URL="https://api.mistral.ai/v1/chat/completions"
GLM_URL="https://api.z.ai/api/paas/v4/chat/completions"

SYSTEM_PROMPT='Classify the user programming task into exactly one tier:
[TIER_1_SIMPLE]: quick syntax, single function, lookup, explanation
[TIER_2_MEDIUM]: multi-file changes, bug fixing, medium edits
[TIER_3_COMPLEX]: deep architectural reasoning, tricky algorithms, math/threading
Answer ONLY with the tag.'

# ------------------------------------------------------------
# Check environment
# ------------------------------------------------------------

echo "Classifier benchmark starting..."
echo

if [[ -z "${MISTRAL_API_KEY:-}" ]]; then
    echo "ERROR: MISTRAL_API_KEY is not set."
    exit 1
fi

if [[ -z "${ZAI_API_KEY:-}" ]]; then
    echo "ERROR: ZAI_API_KEY is not set."
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl is not installed."
    exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "ERROR: jq is not installed."
    echo "Install with: pacman -S jq"
    exit 1
fi

echo "Mistral model: $MISTRAL_MODEL"
echo "GLM model:     $GLM_MODEL"
echo

# ------------------------------------------------------------
# Test cases
#
# Stored as:
#   expected<TAB>question
# ------------------------------------------------------------

TESTS=(
$'TIER_1_SIMPLE\tWhat does std::move do in C++?'
$'TIER_1_SIMPLE\tHow do I check whether a std::string is empty?'
$'TIER_1_SIMPLE\tWrite a C++ function that returns the maximum of two integers.'
$'TIER_1_SIMPLE\tWhat is the difference between const int* and int* const?'
$'TIER_1_SIMPLE\tHow do I iterate over a std::vector<int> using a range-based for loop?'
$'TIER_1_SIMPLE\tWhat does HTTP status code 404 mean?'
$'TIER_1_SIMPLE\tWrite a PostgreSQL query that selects all users where age > 18.'
$'TIER_1_SIMPLE\tHow do I center a div horizontally using CSS?'

$'TIER_2_MEDIUM\tI have a C++ class split between Foo.h and Foo.cpp. I want to rename Foo::process() to Foo::execute() and update every caller. What changes do I need to make?'
$'TIER_2_MEDIUM\tMy Node.js API returns HTTP 500 when a user submits an empty email address. Find the likely problem and suggest the fix.'
$'TIER_2_MEDIUM\tRefactor this C++ function into two functions: one responsible for parsing the input and another responsible for validating it.'
$'TIER_2_MEDIUM\tI changed a PostgreSQL table by renaming a column, and now several queries in my application are failing. How should I systematically update the affected code?'
$'TIER_2_MEDIUM\tMy Svelte component has three different places that independently fetch the same user data. Refactor it so the data is fetched once and shared between the components.'
$'TIER_2_MEDIUM\tA C++ program has a memory leak somewhere across several classes using raw pointers. How would you approach finding and fixing it?'
$'TIER_2_MEDIUM\tI have five C++ files implementing a small feature. I want to replace the current logging calls with a new Logger class. What files and code would likely need to change?'

$'TIER_3_COMPLEX\tI have a multithreaded C++ application where two worker threads occasionally deadlock, but the deadlock cannot be reproduced reliably. Analyze how you would diagnose the lock ordering problem and redesign the synchronization to eliminate the possibility of deadlock.'
$'TIER_3_COMPLEX\tDesign a lock-free multi-producer, multi-consumer queue in C++ using atomics. Explain the memory-ordering requirements and the ABA problem.'
$'TIER_3_COMPLEX\tI need to redesign my C++ application so that plugins can be loaded dynamically at runtime without the core application depending on concrete plugin implementations. Design the architecture and explain the ownership and ABI boundaries.'
$'TIER_3_COMPLEX\tMy neural-network training becomes unstable only after several thousand iterations. The loss sometimes explodes to infinity and then becomes NaN. Develop a systematic diagnosis covering gradients, initialization, learning rate, floating-point behavior, and optimizer state.'
$'TIER_3_COMPLEX\tDesign an algorithm for detecting cycles in a directed graph where the graph may contain millions of nodes and edges. Compare the possible approaches in terms of time, memory, and parallelization.'
$'TIER_3_COMPLEX\tI need a thread-safe cache supporting concurrent reads and writes, bounded memory, LRU eviction, and asynchronous refresh without blocking readers. Design the architecture and synchronization strategy.'
$'TIER_3_COMPLEX\tDesign a C++ memory allocator optimized for millions of small allocations from multiple threads. Explain the data structures, synchronization strategy, fragmentation behavior, and tradeoffs.'
$'TIER_3_COMPLEX\tMy application occasionally produces corrupted data even though ThreadSanitizer reports no data races. Develop a systematic investigation strategy for finding possible memory-ordering, lifetime, undefined-behavior, and cache-coherency-related problems.'

$'TIER_1_SIMPLE\tFix this C++ segmentation fault. The crash occurs when processing an empty vector. The relevant function is about 20 lines long.'
$'TIER_2_MEDIUM\tI have a C++ function that has grown to 200 lines. Refactor it into smaller functions while preserving its behavior.'
$'TIER_3_COMPLEX\tMy application has a race condition between two threads accessing a shared std::unordered_map. How should I fix it?'
$'TIER_2_MEDIUM\tI need to change the API of a C++ class used by 12 other classes. How should I approach the refactoring?'
$'TIER_2_MEDIUM\tImplement a binary search tree in C++ that supports insertion, deletion, and lookup.'
$'TIER_3_COMPLEX\tImplement a thread-safe singleton in modern C++.'
$'TIER_3_COMPLEX\tMy C++ application has a deadlock caused by two mutexes being acquired in different orders. How should I fix it?'
$'TIER_2_MEDIUM\tOptimize this SQL query that joins four tables and takes 8 seconds to execute.'
$'TIER_3_COMPLEX\tI need to convert a synchronous Node.js API to asynchronous processing using a job queue. What architectural changes are required?'
)

# ------------------------------------------------------------
# API functions
# ------------------------------------------------------------

query_mistral() {
    local question="$1"

    curl -sS \
        --connect-timeout 10 \
        --max-time 30 \
        --retry 2 \
        --retry-delay 2 \
        --retry-all-errors \
        "$MISTRAL_URL" \
        -H "Authorization: Bearer ${MISTRAL_API_KEY}" \
        -H "Content-Type: application/json" \
        -d "$(jq -n \
            --arg model "$MISTRAL_MODEL" \
            --arg system "$SYSTEM_PROMPT" \
            --arg question "$question" \
            '{
                model: $model,
                temperature: 0,
                max_tokens: 20,
                messages: [
                    {role: "system", content: $system},
                    {role: "user", content: $question}
                ]
            }')" \
        | jq -r '.choices[0].message.content // "API_ERROR"'
}

query_glm() {
    local question="$1"

    curl -sS \
        --connect-timeout 10 \
        --max-time 30 \
        --retry 2 \
        --retry-delay 2 \
        --retry-all-errors \
        "$GLM_URL" \
        -H "Authorization: Bearer ${ZAI_API_KEY}" \
        -H "Content-Type: application/json" \
        -d "$(jq -n \
            --arg model "$GLM_MODEL" \
            --arg system "$SYSTEM_PROMPT" \
            --arg question "$question" \
            '{
                model: $model,
                temperature: 0,
                max_tokens: 100,
                reasoning_effort: "low",
                messages: [
                    {role: "system", content: $system},
                    {role: "user", content: $question}
                ]
            }')" \
        | jq -r '.choices[0].message.content // "API_ERROR"'
}

# ------------------------------------------------------------
# Run
# ------------------------------------------------------------

total=0
mistral_correct=0
glm_correct=0

echo "Running ${#TESTS[@]} test cases..."
echo

for test in "${TESTS[@]}"; do

    expected="${test%%$'\t'*}"
    question="${test#*$'\t'}"

    ((total++))

    echo "------------------------------------------------------------"
    echo "[$total/${#TESTS[@]}]"
    echo "Expected: $expected"
    echo "Question: $question"
    echo

    echo -n "  Mistral: "
    mistral=$(query_mistral "$question")
    mistral_clean=$(echo "$mistral" | tr -d '[:space:]')
    echo "$mistral_clean"

    echo -n "  GLM:     "
    glm=$(query_glm "$question")
    glm_clean=$(echo "$glm" | tr -d '[:space:]')
    echo "$glm_clean"

    sleep 1

    # Normalize responses:
    # - remove square brackets
    # - remove whitespace
    # - case insensitive
    mistral_normalized=$(echo "$mistral_clean" | tr -d '[][:space:]' | tr '[:upper:]' '[:lower:]')
    glm_normalized=$(echo "$glm_clean" | tr -d '[][:space:]' | tr '[:upper:]' '[:lower:]')
    expected_normalized=$(echo "$expected" | tr -d '[][:space:]' | tr '[:upper:]' '[:lower:]')

    if [[ "$mistral_normalized" == "$expected_normalized" ]]; then
        ((mistral_correct++))
        echo "  Mistral: OK"
    else
        echo "  Mistral: WRONG"
    fi

    if [[ "$glm_normalized" == "$expected_normalized" ]]; then
        ((glm_correct++))
        echo "  GLM:     OK"
    else
        echo "  GLM:     WRONG"
    fi

done

# ------------------------------------------------------------
# Summary
# ------------------------------------------------------------

echo
echo "============================================================"
echo "SUMMARY"
echo "============================================================"

printf "Tests:   %d\n" "$total"
printf "Mistral: %d/%d\n" "$mistral_correct" "$total"
printf "GLM:     %d/%d\n" "$glm_correct" "$total"

echo "============================================================"
