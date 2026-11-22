# Gong LoRa System — Engineering Instructions

## Project
ESP32 firmware project using C++, Arduino,
PlatformIO, LoRa, RTC, I2S audio and NVS.

## Agent roles

### Primary Developer
Model: anthropic/claude-fable-5.2

Responsibilities:
- Analyze existing implementation before editing.
- Implement minimal, targeted changes.
- Preserve established firmware behavior.
- Explain modifications and their risks.

### Independent Reviewer
Model: openai/gpt-6.2-astra

Responsibilities:
- Independently review proposed changes.
- Find logic errors, memory bugs and race conditions.
- Review FreeRTOS concurrency and watchdog risks.
- Analyze NVS persistence and recovery.
- Identify regressions and missing tests.
- Do not modify project files during review.

## Mandatory safety rules

1. Never flash ESP32 without explicit user approval.
2. Never erase NVS, LittleFS or persistent schedules.
3. Never modify hardware pin assignments without approval.
4. Never modify config.h without explicit approval.
5. Never execute git reset --hard, git clean,
   force push or destructive Git operations.
6. Preserve existing working functionality.
7. Do not overwrite uncommitted user changes.
8. Never expose credentials or private configuration.
9. Ask for approval before destructive operations.

## Development workflow

1. Inspect relevant source files.
2. Identify dependencies and potential regressions.
3. Explain the proposed change.
4. Make minimal modifications after authorization.
5. Run relevant host tests.
6. Run PlatformIO build when available.
7. Review the resulting Git diff.
8. Request independent review.
9. Report outstanding risks and test limitations.

## Critical subsystems

- Active-day schedule state
- NVS persistence and recovery
- RTC / DS3231
- I2S audio / MAX98357A
- LoRa communication
- WiFi and web interface
- FreeRTOS tasks and synchronization

## Verified host test command

python3 tests/run_active_day_state_tests.py \
  --build-dir /tmp/gong-active-day-tests

Host test success does not prove hardware correctness.

## Git policy

Current development branch: standalone-max

Do not commit or push without explicit user approval.
Preserve existing stashes and checkpoints.
