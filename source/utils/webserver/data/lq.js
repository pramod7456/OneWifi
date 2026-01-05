// lq.js

// --------------------
// Format metric label for display
// --------------------
function formatLabel(key) {
  if (key.startsWith('DOWNLINK')) return 'DNLINK ' + key.split('_')[1];
  if (key.startsWith('UPLINK')) return 'UPLINK ' + key.split('_')[1];
  return key;
}
function formatScoreLabel(key) {
  if (key === 'DOWNLINK_Score') return 'DNLINK SCORE';
  if (key === 'UPLINK_Score') return 'UPLINK SCORE';
  if (key === 'Score') return 'SCORE'; // Aggregate score
  return key;
}

// --------------------
// Fetch link parameters from server
// --------------------
export async function fetchLinkParams(userEditing = false, lastLocalChange = 0) {
  if (userEditing && (Date.now() - lastLocalChange < 1500)) return;
  try {
    const r = await fetch('/linkparams.json', { cache: 'no-store' });
    if (!r.ok) return;
    return await r.json();
  } catch (e) {
    console.error('fetchLinkParams error:', e);
  }
}

// --------------------
// VAP name helper
// --------------------
function getVapName(vapIndex) {
  const map = {
    0: 'private_ssid_2g',
    1: 'private_ssid_5g',
    2: 'iot_ssid_2g',
    3: 'iot_ssid_5g',
    4: 'hotspot_open_2g',
    5: 'hotspot_open_5g',
    6: 'lnf_psk_2g',
    7: 'lnf_psk_5g',
    8: 'hotspot_secure_2g',
    9: 'hotspot_secure_5g',
    10: 'lnf_radius_2g',
    11: 'lnf_radius_5g',
    12: 'mesh_backhaul_2g',
    13: 'mesh_backhaul_5g',
    14: 'mesh_sta_2g',
    15: 'mesh_sta_5g',
    16: 'private_ssid_6g',
    17: 'iot_ssid_6g',
    18: 'hotspot_open_6g',
    19: 'lnf_psk_6g',
    20: 'hotspot_secure_6g',
    22: 'mesh_backhaul_6g',
    23: 'mesh_sta_6g'
  };
  return map[vapIndex] || `vap_${vapIndex}`;
}
const LINE_WIDTHS = {
  metric: 2,   // SNR / PER / PHY
  score: 5     // Score
};
// --------------------
// Line styles and colors
// --------------------
const LINE_STYLES = {
  downlink: { dash: 'solid'},
  uplink: { dash: 'dash' },
  aggregate: { dash: 'solid'}
};

const COLORS_METRICS = {
  DNLINK_SNR: '#1f77b4',
  DNLINK_PER: '#2ca02c',
  DNLINK_PHY: '#9467bd',
  DNLINK_Score: '#d62728',
  UPLINK_SNR: '#1f77b4',
  UPLINK_PER: '#2ca02c',
  UPLINK_PHY: '#9467bd',
  UPLINK_Score: '#d62728',
  SNR: '#1f77b4',
  PER: '#2ca02c',
  PHY: '#9467bd',
  Score: '#d62728'
};
function getMetricColor(key) {
  return COLORS_METRICS[key] || COLORS_METRICS[
    key.includes('_SNR') ? 'SNR' :
    key.includes('_PER') ? 'PER' :
    key.includes('_PHY') ? 'PHY' :
    key.includes('Score') ? 'Score' :
    '#000000'
  ];
}

// --------------------
// Get selected metrics from checkboxes
// --------------------
function getSelectedMetrics() {
  return {
    downlink: [...document.querySelectorAll('#downlinkDropdown input:checked')].map(cb => cb.value),
    uplink:   [...document.querySelectorAll('#uplinkDropdown input:checked')].map(cb => cb.value),
    aggregate: document.querySelector('#aggregateCheckbox')?.checked === true
  };
}

// --------------------
// Render charts
// --------------------
export function renderLinkQualityChart(data) {
  const container = document.getElementById('LeftGridContainer');
  container.innerHTML = '';

  if (!data || !data.Devices) return;

  // --------------------
  // Read UI selections
  // --------------------
  const selected = {
    downlink: [...document.querySelectorAll('#downlinkDropdown input:checked')].map(cb => cb.value),
    uplink:   [...document.querySelectorAll('#uplinkDropdown input:checked')].map(cb => cb.value),
    aggregate: document.querySelector('#aggregateCheckbox')?.checked === true
  };

  // --------------------
  // Helper: metric families (PER / PHY / SNR)
  // --------------------
  function getMetricFamilies() {
    const families = new Set();
    [...selected.downlink, ...selected.uplink].forEach(k => {
      if (k.includes('_PER')) families.add('PER');
      if (k.includes('_PHY')) families.add('PHY');
      if (k.includes('_SNR')) families.add('SNR');
    });
    return [...families];
  }

  const metricFamilies = getMetricFamilies();

  // --------------------
  // Render per device
  // --------------------
  data.Devices.forEach(dev => {
    if (!dev.LinkQuality || !dev.Time) return;

    const traces = [];
    const div = document.createElement('div');
    div.style.height = '400px';
    div.style.width = '100%'; // responsive width
    container.appendChild(div);

    // ==========================================================
    // AGGREGATE MODE
    // ==========================================================
    if (selected.aggregate) {
      metricFamilies.forEach(type => {
        const val = dev.LinkQuality[type];
        if (!val) return;

        traces.push({
          name: type,
          x: dev.Time,
          y: val,
          type: 'scatter',
          mode: 'lines',
         line: {
    ...LINE_STYLES.aggregate,
      color: getMetricColor(type),
    width: LINE_WIDTHS.metric
  }
        });
      });

      // Aggregate Score
      if (dev.LinkQuality.Score) {
        traces.push({
name: formatScoreLabel('Score'),          
x: dev.Time,
          y: dev.LinkQuality.Score,
          type: 'scatter',
          mode: 'lines',
         line: {
    ...LINE_STYLES.aggregate,
    width: LINE_WIDTHS.score,
      color: COLORS_METRICS.Score
  }
        });
      }
    }
    // ==========================================================
    // NON-AGGREGATE MODE
    // ==========================================================
    else {
      // Downlink metrics
      selected.downlink.forEach(key => {
        const val = dev.LinkQuality[key];
        if (!val) return;

        traces.push({
          name: formatLabel(key),
           x: dev.Time,
          y: val,
          type: 'scatter',
          mode: 'lines',
         line: {
    ...LINE_STYLES.downlink,
      color: getMetricColor(key),
    width: LINE_WIDTHS.metric
  }
        });
      });

      // Uplink metrics
      selected.uplink.forEach(key => {
        const val = dev.LinkQuality[key];
        if (!val) return;

        traces.push({
            name: formatLabel(key),
          x: dev.Time,
          y: val,
          type: 'scatter',
          mode: 'lines',
         line: {
    ...LINE_STYLES.uplink,
      color: getMetricColor(key),
    width: LINE_WIDTHS.metric
  }
        });
      });

      // Downlink Score
      if (selected.downlink.length > 0 && dev.LinkQuality.DOWNLINK_Score) {
        traces.push({
name: formatScoreLabel('DOWNLINK_Score'), 
          x: dev.Time,
          y: dev.LinkQuality.DOWNLINK_Score,
          type: 'scatter',
          mode: 'lines',
       line: {
      ...LINE_STYLES.downlink,
    width: LINE_WIDTHS.score,
      color: COLORS_METRICS.DNLINK_Score
    }  
      });
      }

      // Uplink Score
      if (selected.uplink.length > 0 && dev.LinkQuality.UPLINK_Score) {
        traces.push({
name: formatScoreLabel('UPLINK_Score'), 
          x: dev.Time,
          y: dev.LinkQuality.UPLINK_Score,
          type: 'scatter',
          mode: 'lines',
   line: {
      ...LINE_STYLES.uplink,
    width: LINE_WIDTHS.score,
      color: COLORS_METRICS.UPLINK_Score
    }
        });
      }
    }
// --------------------
// Determine the latest score
// --------------------
let latestScore = null;

if (selected.aggregate && dev.LinkQuality.Score) {
  // Aggregate → latest score rounded to 3 decimals
  const scoreArr = dev.LinkQuality.Score;
  latestScore = scoreArr[scoreArr.length - 1].toFixed(3);
} else if (selected.downlink.length > 0 && selected.uplink.length > 0) {
  // Both downlink and uplink → show latest of each rounded to 3 decimals
  const dl = dev.LinkQuality.DOWNLINK_Score?.[dev.LinkQuality.DOWNLINK_Score.length - 1] ?? 0;
  const ul = dev.LinkQuality.UPLINK_Score?.[dev.LinkQuality.UPLINK_Score.length - 1] ?? 0;
  latestScore = `Downlink: ${dl.toFixed(3)}, Uplink: ${ul.toFixed(3)}`;
} else if (selected.downlink.length > 0) {
  const dl = dev.LinkQuality.DOWNLINK_Score?.[dev.LinkQuality.DOWNLINK_Score.length - 1] ?? 0;
  latestScore = dl.toFixed(3);
} else if (selected.uplink.length > 0) {
  const ul = dev.LinkQuality.UPLINK_Score?.[dev.LinkQuality.UPLINK_Score.length - 1] ?? 0;
  latestScore = ul.toFixed(3);
}

// --------------------
// Build layout
// ---------------
const vapName = dev.VapIndex !== undefined ? getVapName(dev.VapIndex) : 'unknown';
const layout = {
  title: {
   text: `
      <b>Link Quality:</b>${dev.MAC}<br>
      <b>Score:</b> ${latestScore ?? '-'} &nbsp;&nbsp; <b>VAP:</b> ${vapName}
    `, 
   x: 0.5,
    xanchor: 'center'
  },
  xaxis: {
    title: 'Time',
    tickangle: -45 // slanted labels
  },
  yaxis: {
    title: 'Score',
    range: [0, 1],
    tick0: 0,
    dtick: 0.1,
    fixedrange: true
  },
  height: 400,
  autosize: true,
  uirevision: 'keep',
  margin: { t: 100, l: 60, r: 40, b: 60 } // extra top margin for multi-line title
};

    // --------------------
    // Render chart while preserving scroll
    // --------------------
    //const scrollY = window.scrollY;
    Plotly.react(div, traces, layout, { responsive: true }).then(() => {
      //window.scrollTo(0, scrollY);
    });
  });
}

// --------------------
// Render alarms table
// --------------------
function updateAlarmDot(show) {
  const dot = document.getElementById('alarmDot');
  if (dot) dot.style.display = show ? 'block' : 'none';
}

export function renderAlarms(data) {
  const table = document.getElementById('Alarms');
  table.innerHTML = `<tr><th>Device</th><th>Description</th><th>Time</th></tr>`;
  let hasAlarm = false;

  data.Devices?.forEach(dev => {
    dev.LinkQuality?.Alarms?.forEach(a => {
      if (!a) return;
      hasAlarm = true;
      const row = document.createElement('tr');
      row.innerHTML = `<td>${dev.MAC}</td><td>Link Quality Alarm</td><td>${a}</td>`;
      table.appendChild(row);
    });
  });

  updateAlarmDot(hasAlarm);
}

