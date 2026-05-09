<script setup>
const model = defineModel({ required: true });
const slots = defineSlots();
const props = defineProps({
  class: {
    type: String,
    default: ""
  },
  desc: {
    type: String,
    default: null
  },
  id: {
    type: String,
    required: true
  },
  label: {
    type: String,
    default: null
  },
  localePrefix: {
    type: String,
    default: "missing-prefix"
  },
  inverseValues: {
    type: Boolean,
    default: false,
  },
  default: {
    type: undefined,
    default: null,
  },
  compact: {
    type: Boolean,
    default: false,
  },
  showDefault: {
    type: Boolean,
    default: true,
  }
});

// Map the value to boolean representation if possible, otherwise return null.
const mapToBoolRepresentation = (value) => {
  // Try literal values first
  if (value === true || value === false) {
    return { possibleValues: [true, false], value: value };
  }
  if (value === 1 || value === 0) {
    return { possibleValues: [1, 0], value: value };
  }

  const stringPairs = [
    ["true", "false"],
    ["1", "0"],
    ["enabled", "disabled"],
    ["enable", "disable"],
    ["yes", "no"],
    ["on", "off"]
  ];

  value = `${value}`.toLowerCase().trim();
  for (const pair of stringPairs) {
    if (value === pair[0] || value === pair[1]) {
      return { possibleValues: pair, value: value };
    }
  }

  return null;
}

// Determine the true/false values for the checkbox
const checkboxValues = (() => {
  const mappedValues = (() => {
    const boolValues = mapToBoolRepresentation(model.value);
    if (boolValues !== null) {
      return boolValues.possibleValues;
    }

    // Return fallback if nothing matches
    console.error(`Checkbox value ${model.value} for "${props.id}" did not match any acceptable pattern!`);
    return ["true", "false"];
  })();

  const truthyIndex = props.inverseValues ? 1 : 0;
  const falsyIndex = props.inverseValues ? 0 : 1;
  return { truthy: mappedValues[truthyIndex], falsy: mappedValues[falsyIndex] };
})();
const parsedDefaultPropValue = (() => {
  const boolValues = mapToBoolRepresentation(props.default);
  if (boolValues !== null) {
    // Convert truthy to true/false.
    return boolValues.value === boolValues.possibleValues[0];
  }

  return null;
})();

const labelField = props.label ?? `${props.localePrefix}.${props.id}`;
const descField = props.desc ?? `${props.localePrefix}.${props.id}_desc`;
const showDesc = props.desc !== "" || Object.entries(slots).length > 0;
const showDefValue = props.showDefault && parsedDefaultPropValue !== null;
const defValue = parsedDefaultPropValue ? "_common.enabled_def_cbox" : "_common.disabled_def_cbox";
</script>

<template>
  <div :class="[props.class, { 'checkbox-field--compact': props.compact }]" :data-setting-key="props.id" class="checkbox-field">
    <label :for="props.id" class="checkbox-field-control flex items-start gap-2">
      <span class="checkbox-field-toggle">
        <input type="checkbox"
               class="checkbox-field-input"
               :id="props.id"
               v-model="model"
               :true-value="checkboxValues.truthy"
               :false-value="checkboxValues.falsy" />
      </span>
      <span class="checkbox-field-copy flex min-w-0 flex-col gap-1">
        <span class="checkbox-field-title-row flex flex-wrap items-baseline gap-x-2 gap-y-1">
          <span class="text-silver cursor-pointer checkbox-field-label leading-snug">
            {{ $t(labelField) }}
          </span>
          <span class="text-xs text-storm checkbox-field-default" v-if="showDefValue">
            {{ $t(defValue) }}
          </span>
        </span>
        <span class="text-sm text-storm checkbox-field-desc" v-if="showDesc">
          {{ $t(descField) }}
          <slot></slot>
        </span>
      </span>
    </label>
  </div>
</template>
