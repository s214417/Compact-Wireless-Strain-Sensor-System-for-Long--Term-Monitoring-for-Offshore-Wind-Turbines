# Claude Code Project Explanation Prompt

Use this prompt with Claude Code while opened at the `Custom_PCB_V3` project root.

This project runs on an EFR32MG22E and is a low-power PCB designed to measure a Wheatstone bridge with 4 sensors. It reads the bridge through ADC sampling and sends the ADC value to Home Assistant (HA).

---

You are analyzing this embedded Zigbee project.

Task:
1. Read every meaningful source file in the project.
2. Explain each function in plain language.
3. Explain how the whole program works from startup to main loop, including Zigbee bring-up, sensor sampling, low-power behavior, and any callback flow.
4. Call out the order of execution, important data flow, state machines, timers, and callback relationships.
5. Identify generated code and build output, but do not spend time explaining files under `autogen/`, `cmake_gcc/build/`, or other generated directories unless they are directly needed to understand the program.
6. If a function is a wrapper, callback, or helper, say what it wraps or helps and why it exists.
7. If something is unclear from the code, say so instead of guessing.

Output format:
1. Start with a short project overview.
2. Then give a file-by-file explanation of the important source files.
3. For each file, list every function and explain:
   - what it does
   - when it runs
   - what it depends on
   - what state it changes
4. End with a step-by-step explanation of the full program flow.
5. Finish with a short list of important design choices, risks, or low-power behaviors worth knowing.

Focus areas for this project:
- `main.c`: startup and main loop
- `app.c` / `app.h`: application behavior and glue code
- `zigbee.c` / `zigbee.h`: network join/rejoin logic, callbacks, and sleep decisions
- `wheatstone.c` / `wheatstone.h`: bridge power control and ADC sampling

Use a practical engineering tone and be explicit about control flow. Do not summarize too early; inspect the code deeply enough to explain the actual behavior.

If helpful, produce the explanation in Markdown so it is easy to read and reuse.

**Store Q&A Output**

At the end of the analysis, create a file named `claude_code_explain_project_QA.md` in the project root that stores the user's original question and the answers produced by this analysis. Each entry must include a timestamp, the original question text, and the full answer content. If the file already exists, append a new entry with a timestamp rather than overwriting.