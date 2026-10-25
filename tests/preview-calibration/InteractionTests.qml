import QtQuick
import QtTest
import "Fixtures.js" as Fixtures

TestCase {
  id: tests
  name: "NativeCalibration"
  property var harness
  property var calibration
  property bool ready: false
  when: ready
  Timer { interval: 700; running: true; onTriggered: tests.ready = true }

  function init() {
    harness.selectScenario("ready")
    wait(60)
  }

  // QtTest's own failure lines do not reach the harness output; name the test.
  property int failuresSeen: 0
  function cleanup() {
    if (qtest_results.failCount > failuresSeen) console.log("Native calibration test failed: " + qtest_results.functionName)
    failuresSeen = qtest_results.failCount
  }

  function control(name) {
    var result = findChild(calibration, name)
    verify(result !== null, "Missing native control " + name)
    return result
  }

  function numericFor(item) {
    while (item && item.inputError === undefined) item = item.parent
    verify(item, "No CalibrationNumeric around the control")
    return item
  }

  function typeInto(field, text) {
    field.forceActiveFocus()
    field.selectAll()
    for (var i = 0; i < text.length; i++) keyClick(text[i])
  }

  function check(title) {
    var rows = calibration.checks.filter(function(row) { return row.title === title })
    return rows.length ? rows[0] : null
  }

  function test_ready_invites_a_test() {
    compare(calibration.bannerTitle, "Test the draft")
    verify(calibration.bannerDetail.indexOf("Nothing is saved") >= 0)
    compare(control("calibration_checks").visible, false)
    compare(control("calibration_test").text, "Test")
    mouseClick(control("calibration_test"))
    compare(harness.testRequests, 1)
  }

  function test_running_marks_each_requirement() {
    harness.selectScenario("running")
    verify(calibration.bannerTitle.indexOf("Testing") === 0)
    compare(calibration.checkMark(check("Identity")), "✓")
    compare(calibration.checkMark(check("Liveness")), "✗")
    compare(calibration.checkText(check("Liveness")), "0.641 < 0.850")
    compare(calibration.checkText(check("Streak")), "0 / 8")
    compare(calibration.checkMark(check("Streak")), "")
    compare(calibration.gateText(), "Liveness below requirement")
    verify(control("calibration_checks").visible)
  }

  function test_accepted_is_the_largest_text() {
    harness.selectScenario("pass")
    compare(calibration.bannerTitle, "✓ Accepted")
    verify(calibration.bannerDetail.indexOf("As Lock would") === 0)
    verify(calibration.bannerDetail.indexOf("1.4 s") > 0)
    compare(calibration.bannerColor, calibration.goodColor)
    var title = control("calibration_result_title")
    verify(title.font.pixelSize > control("calibration_result_detail").font.pixelSize)
    // Afterwards the checks show the best reached, against what was required.
    compare(calibration.checkText(check("Streak")), "8 ≥ 8")
    compare(check("Face size"), null)
  }

  function test_not_accepted_names_the_reason() {
    harness.selectScenario("reject")
    compare(calibration.bannerTitle, "✗ Not accepted")
    compare(calibration.bannerDetail, "Face matched, but liveness could not be verified")
    compare(calibration.bannerColor, calibration.badColor)
    compare(calibration.checkMark(check("Liveness")), "✗")
    compare(calibration.checkMark(check("Identity")), "✓")
    // After the scan, a streak that never completed is the failure.
    compare(calibration.checkMark(check("Streak")), "✗")
    compare(calibration.checkText(check("Streak")), "2 < 8")
    harness.selectScenario("nomatch")
    compare(calibration.checkMark(check("Identity")), "✗")
  }

  function test_head_turn_prompt_is_shown() {
    harness.selectScenario("turn")
    compare(calibration.gateText(), "← Turn your head left")
  }

  function test_state_colours_never_dim_the_text() {
    // A muted theme: accent and urgent darker than the text.
    calibration.accent = "#798186"
    calibration.background = "#101315"
    verify(calibration.contrast(calibration.accent, calibration.background) < 6)
    compare(calibration.goodColor, calibration.foreground)
    // A vivid accent on the same page is used as is.
    calibration.accent = "#7dcfff"
    compare(calibration.goodColor, calibration.accent)
    harness.selectScenario("weaker")
    verify(control("calibration_weaker_warning").visible)
    verify(numericFor(control("slider_liveness_threshold")).reducedSecurity)
  }

  function test_zero_and_missing_measurements() {
    compare(calibration.formatted(0, 3), "0.000")
    compare(calibration.formatted(null, 3), "—")
    compare(calibration.formatted(NaN, 3), "—")
    compare(calibration.checkMark({ current: null, required: 0.5 }), "")
    compare(calibration.checkText({ current: undefined, required: 0.5, digits: 3 }), "— / 0.500")
  }

  function test_editing_clears_a_result_from_another_draft() {
    harness.selectScenario("pass")
    harness.edit("threshold_rgb", 0.6)
    compare(calibration.bannerTitle, "Test the draft")
    compare(calibration.hasResult, false)
  }

  function test_controls_are_locked_while_testing() {
    harness.selectScenario("running")
    compare(control("calibration_test").text, "Stop")
    compare(numericFor(control("slider_threshold_rgb")).enabled, false)
    compare(control("calibration_place_sudo").enabled, false)
    compare(control("calibration_defaults").enabled, false)
    compare(control("calibration_refresh").enabled, false)
    mouseClick(control("calibration_test"))
    compare(harness.testState, "idle")
  }

  function test_place_sets_the_scan_time() {
    var sudo = control("calibration_place_sudo")
    compare(sudo.text, "sudo 4 s")
    verify(control("calibration_place_lock").selected)
    mouseClick(sudo)
    compare(harness.testPlace, "sudo")
    verify(sudo.selected)
    harness.edit("timeout_sudo", 9)
    compare(sudo.text, "sudo 9 s")
  }

  function test_weaker_draft_warns_before_apply() {
    compare(control("calibration_weaker_warning").visible, false)
    compare(control("calibration_apply").text, "Apply")
    harness.selectScenario("weaker")
    var warning = control("calibration_weaker_warning")
    verify(warning.visible)
    compare(calibration.weakerKeys.map(calibration.titleOf).join(", "), "Liveness, Frames")
    compare(control("calibration_apply").text, "Apply weaker settings")
    mouseClick(control("calibration_defaults"))
    compare(warning.visible, false)
  }

  function test_keyboard_slider_snaps_and_preserves_other_values() {
    var previous = Fixtures.copy(harness.draft)
    harness.edit("threshold_rgb", 0.543721)
    var slider = control("slider_threshold_rgb")
    slider.forceActiveFocus()
    wait(30)
    keyClick(Qt.Key_Right)
    fuzzyCompare(harness.draft.threshold_rgb, 0.55, 0.00000001)
    keyClick(Qt.Key_Home)
    fuzzyCompare(harness.draft.threshold_rgb, harness.metadata.bounds.threshold_rgb.min, 0.00000001)
    keyClick(Qt.Key_End)
    fuzzyCompare(harness.draft.threshold_rgb, harness.metadata.bounds.threshold_rgb.max, 0.00000001)
    compare(harness.draft.threshold, previous.threshold)
    compare(harness.saved.threshold_rgb, previous.threshold_rgb)
    verify(slider.activeFocus)
  }

  function test_pointer_slider_snaps() {
    var slider = control("slider_threshold_rgb")
    wait(60)
    var before = harness.draft.threshold_rgb
    mouseClick(slider, slider.width * 0.437, slider.height / 2)
    verify(harness.draft.threshold_rgb !== before)
    verify(slider.activeFocus)
    var bound = harness.metadata.bounds.threshold_rgb
    var steps = (harness.draft.threshold_rgb - bound.min) / bound.step
    fuzzyCompare(steps, Math.round(steps), 0.000001)
    compare(harness.saved.threshold_rgb, 0.55)
  }

  function test_defaults_and_revert_do_not_apply() {
    harness.selectScenario("weaker")
    var saved = JSON.stringify(harness.saved)
    mouseClick(control("calibration_defaults"))
    compare(harness.draft.liveness_threshold, 0.85)
    compare(JSON.stringify(harness.saved), saved)
    harness.edit("liveness_threshold", 0.9)
    mouseClick(control("calibration_revert"))
    compare(JSON.stringify(harness.draft), saved)
    compare(harness.applyAttempts, 0)
  }

  function test_apply_fixture_is_inert_and_keeps_draft() {
    harness.selectScenario("weaker")
    var saved = JSON.stringify(harness.saved)
    var draft = JSON.stringify(harness.draft)
    var apply = control("calibration_apply")
    verify(apply.focusable)
    apply.forceActiveFocus()
    keyClick(Qt.Key_Return)
    compare(harness.applyAttempts, 1)
    compare(JSON.stringify(harness.saved), saved)
    compare(JSON.stringify(harness.draft), draft)
    verify(harness.notice.indexOf("disabled") >= 0)
  }

  function test_conflict_disables_apply_until_revert() {
    harness.selectScenario("conflict")
    var apply = control("calibration_apply")
    compare(apply.enabled, false)
    verify(control("calibration_notice").text.indexOf("changed elsewhere") >= 0)
    mouseClick(control("calibration_revert"))
    compare(harness.conflicted, false)
    compare(harness.draft.threshold_rgb, 0.58)
    compare(harness.applyAttempts, 0)
  }

  function test_decimal_entry_settles_on_focus_loss() {
    var field = control("number_threshold_rgb")
    typeInto(field, "0.62")
    control("calibration_back").forceActiveFocus()
    fuzzyCompare(harness.draft.threshold_rgb, 0.62, 0.00000001)
    // Out of range: Qt never reports editingFinished, so leaving the field
    // must flag it and show the draft value rather than the rejected text.
    typeInto(field, "0.2")
    control("calibration_refresh").forceActiveFocus()
    fuzzyCompare(harness.draft.threshold_rgb, 0.62, 0.00000001)
    compare(field.text, "0.62")
    compare(numericFor(field).inputError, "Out of range")
    typeInto(field, "0.3")
    keyClick(Qt.Key_Return)
    fuzzyCompare(harness.draft.threshold_rgb, 0.30, 0.00000001)
    compare(numericFor(field).inputError, "")
  }

  function test_ir_hides_liveness_and_shows_the_attention_floor() {
    harness.selectScenario("ir")
    wait(40)
    compare(calibration.infrared, true)
    compare(findChild(calibration, "slider_liveness_threshold"), null)
    compare(check("Liveness"), null)
    compare(calibration.checkText(check("Streak")), "2 / 3")
    harness.selectScenario("ir-pass")
    wait(40)
    var frames = numericFor(control("number_match_frames"))
    compare(frames.note, "Attention on: scans use at least 3")
    harness.edit("match_frames", 1)
    compare(frames.reducedSecurity, false)
    compare(calibration.reduced, false)
    harness.edit("match_frames", 4)
    compare(frames.note, "")
    // Hidden webcam rows cannot raise a warning on an IR camera.
    harness.edit("liveness_threshold", 0.6)
    compare(calibration.reduced, false)
    var withoutAttention = Fixtures.copy(harness.metadata)
    withoutAttention.attention = false
    harness.metadata = withoutAttention
    harness.edit("match_frames", 1)
    compare(frames.note, "")
    compare(frames.reducedSecurity, true)
    compare(calibration.reduced, true)
  }

  function test_tab_focus_and_labels() {
    harness.selectScenario("weaker")
    var back = control("calibration_back")
    back.forceActiveFocus()
    keyClick(Qt.Key_Tab)
    verify(!back.activeFocus)
    verify(control("calibration_fixture_label").visible)
    verify(control("calibration_draft_label").visible)
  }

  function cleanupTestCase() {
    console.log("Native calibration interaction tests: " + qtest_results.passCount + " passed, " + qtest_results.failCount + " failed, " + qtest_results.skipCount + " skipped")
  }
}
