/** Inline-SVG line+area chart for one series. viewBox is a fixed 100-wide
 *  coordinate space that scales to the container via width:100% + a
 *  non-scaling stroke, so it stays crisp at any panel width. */
export default function Sparkline({ series = [], color = '#2DD4BF', height = 44, areaOpacity = 0.15 }) {
  const w = 100;
  const h = height;
  if (!series.length) return <svg width="100%" height={h} />;

  const max = Math.max(...series);
  const min = Math.min(...series);
  const range = max - min || 1;
  const stepX = w / Math.max(series.length - 1, 1);
  const points = series.map((v, i) => [i * stepX, h - ((v - min) / range) * (h - 4) - 2]);
  const linePath = points.map(([x, y], i) => `${i === 0 ? 'M' : 'L'}${x.toFixed(2)},${y.toFixed(2)}`).join(' ');
  const areaPath = `${linePath} L${w},${h} L0,${h} Z`;

  return (
    <svg viewBox={`0 0 ${w} ${h}`} width="100%" height={h} preserveAspectRatio="none">
      <path d={areaPath} fill={color} opacity={areaOpacity} stroke="none" />
      <path d={linePath} fill="none" stroke={color} strokeWidth={1.5} vectorEffect="non-scaling-stroke" />
    </svg>
  );
}
