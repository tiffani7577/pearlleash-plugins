/**
 * Generated from plugforge/contracts/uiContract.js — keep in sync with APVTS NormalisableRange.
 */
/** PlugForge UI↔APVTS normalised conversion (matches juce::NormalisableRange skew). */
export function valueToNormalised(value, range) {
  const min = Number(range.min);
  const max = Number(range.max);
  const skew = Number(range.skew) > 0 ? Number(range.skew) : 1;
  if (!(max > min)) return 0;
  let proportion = (Number(value) - min) / (max - min);
  proportion = Math.min(1, Math.max(0, proportion));
  if (skew !== 1) proportion = Math.pow(proportion, skew);
  return proportion;
}
export function normalisedToValue(normalised, range) {
  const min = Number(range.min);
  const max = Number(range.max);
  const skew = Number(range.skew) > 0 ? Number(range.skew) : 1;
  let proportion = Math.min(1, Math.max(0, Number(normalised) || 0));
  if (skew !== 1 && proportion > 0) proportion = Math.exp(Math.log(proportion) / skew);
  return min + (max - min) * proportion;
}
export function bindingRange(p) {
  return { min: Number(p.min), max: Number(p.max), skew: Number(p.skew) > 0 ? Number(p.skew) : 1 };
}
