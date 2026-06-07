var TIME_WINDOW_MS = 10 * 60 * 1000;

function doGet(e) {
  try {
    var sheet  = SpreadsheetApp.getActiveSheet();
    var temp   = parseFloat(e.parameter.temp);
    var sensor = e.parameter.sensor || "1";
    if (isNaN(temp)) return ContentService.createTextOutput("Failed: Invalid Temp");

    ensureHeaders(sheet);

    var now         = new Date();
    var readingTime = e.parameter.ts
      ? new Date(parseInt(e.parameter.ts) * 1000)
      : now;

    // Is this a buffered (old) reading? If ts is more than 15 min old, yes.
    var isBuffered = (now.getTime() - readingTime.getTime()) > 15 * 60 * 1000;

    // ── SENSOR 1 ──────────────────────────────────────────────────
    if (sensor === "1") {
      sheet.appendRow([readingTime, temp]);
      return ContentService.createTextOutput("Success");
    }

    // ── SENSOR 2 ──────────────────────────────────────────────────
    if (sensor === "2") {
      var lastRow = sheet.getLastRow();
      if (lastRow < 2) {
        sheet.appendRow([readingTime, "", temp, ""]);
        return ContentService.createTextOutput("Success: orphan row (sheet empty)");
      }

      var scanFrom, scanTo;

      if (isBuffered) {
        // Buffered reading: must search the WHOLE sheet because
        // sensor 1 already filled those rows long ago.
        // Read all at once for speed.
        scanFrom = 2;
        scanTo   = lastRow;
      } else {
        // Live reading: only scan the last ~288 rows (~24h at 5min)
        scanFrom = Math.max(2, lastRow - 288);
        scanTo   = lastRow;
      }

      // Batch read for speed (one API call instead of per-cell calls)
      var numRows = scanTo - scanFrom + 1;
      var data    = sheet.getRange(scanFrom, 1, numRows, 4).getValues();

      var bestRow  = -1;
      var bestDiff = TIME_WINDOW_MS + 1;

      for (var i = data.length - 1; i >= 0; i--) {
        var rowTime = data[i][0];
        if (!(rowTime instanceof Date)) continue;

        var diff  = Math.abs(readingTime.getTime() - rowTime.getTime());
        var colB  = data[i][1];
        var colC  = data[i][2];

        var bFilled = (colB !== "" && colB !== null);
        var cFilled = (colC !== "" && colC !== null);

        // Sensor 1 row with empty sensor 2 slot, within time window
        if (diff <= TIME_WINDOW_MS && bFilled && !cFilled) {
          if (diff < bestDiff) {
            bestDiff = diff;
            bestRow  = scanFrom + i; // actual sheet row number
          }
        }
      }

      if (bestRow !== -1) {
        var colBval = sheet.getRange(bestRow, 2).getValue();
        sheet.getRange(bestRow, 3).setValue(temp);
        sheet.getRange(bestRow, 4).setValue(
          Math.round((temp - parseFloat(colBval)) * 100) / 100
        );
        var label = isBuffered ? "buffered fill" : "live match";
        return ContentService.createTextOutput("Success: " + label + " row " + bestRow);
      }

      // No match found — create orphan row
      sheet.appendRow([readingTime, "", temp, ""]);
      return ContentService.createTextOutput("Success: orphan row created");
    }

    return ContentService.createTextOutput("Failed: Unknown sensor");

  } catch (err) {
    return ContentService.createTextOutput("Error: " + err.message);
  }
}

// ================================================================
//  mergeOrphanRows()
//  Trigger this every 10–15 minutes.
//  Catches any S1/S2 rows that still didn't match via doGet.
// ================================================================
function mergeOrphanRows() {
  var sheet   = SpreadsheetApp.getActiveSheet();
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return;

  var data = sheet.getRange(2, 1, lastRow - 1, 4).getValues();

  var s1rows = [];
  var s2rows = [];

  for (var i = 0; i < data.length; i++) {
    var t = data[i][0], b = data[i][1], c = data[i][2];
    if (!(t instanceof Date)) continue;
    var sheetRow = i + 2;
    var bFilled  = (b !== "" && b !== null);
    var cFilled  = (c !== "" && c !== null);

    if (bFilled && !cFilled) s1rows.push({ row: sheetRow, time: t, val: b });
    if (cFilled && !bFilled) s2rows.push({ row: sheetRow, time: t, val: c });
  }

  var rowsToDelete = [];
  var merged = 0;

  s2rows.forEach(function(s2) {
    var bestMatch = null, bestDiff = TIME_WINDOW_MS + 1;

    s1rows.forEach(function(s1) {
      if (s1.used) return;
      var diff = Math.abs(s2.time.getTime() - s1.time.getTime());
      if (diff <= TIME_WINDOW_MS && diff < bestDiff) {
        bestDiff  = diff;
        bestMatch = s1;
      }
    });

    if (bestMatch) {
      bestMatch.used = true;
      sheet.getRange(bestMatch.row, 3).setValue(s2.val);
      sheet.getRange(bestMatch.row, 4).setValue(
        Math.round((s2.val - parseFloat(bestMatch.val)) * 100) / 100
      );
      rowsToDelete.push(s2.row);
      merged++;
    }
  });

  // Delete from bottom up so row numbers stay valid
  rowsToDelete.sort(function(a, b) { return b - a; });
  rowsToDelete.forEach(function(r) { sheet.deleteRow(r); });

  Logger.log("mergeOrphanRows: merged=" + merged + " deleted=" + rowsToDelete.length);
}

function ensureHeaders(sheet) {
  if (sheet.getRange(1, 1).getValue() === "") {
    sheet.getRange(1, 1).setValue("Time and Date");
    sheet.getRange(1, 2).setValue("Temperature Data 1");
    sheet.getRange(1, 3).setValue("Temperature Data 2");
    sheet.getRange(1, 4).setValue("Temperature difference");
  }
}
