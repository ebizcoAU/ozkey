/** Two overlaid series on one shared scale (e.g. MQTT in vs out msgs/sec).
 *  Series B is dashed so the two lines read apart even at a glance. */
export default function DualSparkline({
  seriesA = [],
  seriesB = [],
  colorA = '#38BDF8',
  colorB = '#F59E0B',
  labelA = 'A',
  labelB = 'B',
  height = 54,
}) {
  const w = 100;
  const h = height;
  const all = [...seriesA, ...seriesB];
  if (!all.length) return null;

  const max = Math.max(...all);
  const min = Math.min(...all);
  const range = max - min || 1;
  const mk = (series) => {
    const stepX = w / Math.max(series.length - 1, 1);
    return series
      .map((v, i) => [i * stepX, h - ((v - min) / range) * (h - 4) - 2])
      .map(([x, y], i) => `${i === 0 ? 'M' : 'L'}${x.toFixed(2)},${y.toFixed(2)}`)
      .join(' ');
  };

  return (
    <div>
      <div style={{ display: 'flex', gap: 14, marginBottom: 6, fontSize: 10 }}>
        <span style={{ color: colorA }}>● {labelA}</span>
        <span style={{ color: colorB }}>┄ {labelB}</span>
      </div>
      <svg viewBox={`0 0 ${w} ${h}`} width="100%" height={h} preserveAspectRatio="none">
        <path d={mk(seriesA)} fill="none" stroke={colorA} strokeWidth={1.5} vectorEffect="non-scaling-stroke" />
        <path
          d={mk(seriesB)}
          fill="none"
          stroke={colorB}
          strokeWidth={1.5}
          strokeDasharray="3,2"
          vectorEffect="non-scaling-stroke"
        />
      </svg>
    </div>
  );
}
