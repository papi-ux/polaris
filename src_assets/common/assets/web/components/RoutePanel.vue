<script setup>
// The route a stream takes, host to stream to client, or the host and its paired clients while
// idle. Each node is data the dashboard already has; a node the host has not reported is marked
// unknown rather than filled in.
defineProps({
  title: { type: String, required: true },
  nodes: { type: Array, default: () => [] },
})
const titleId = `mission-route-title-${Math.random().toString(36).slice(2)}`
</script>

<template>
  <aside class="mc-route" :aria-labelledby="titleId" data-mission-route>
    <h3 :id="titleId" class="mc-panel-label"><span class="mc-bracket" aria-hidden="true">&lt;</span> {{ title }} <span class="mc-bracket" aria-hidden="true">&gt;</span></h3>
    <ol class="mc-route-list">
      <li v-for="node in nodes" :key="node.key" class="mc-node" :class="node.state ? `is-${node.state}` : ''" :data-route-node="node.key">
        <span class="mc-node-marker" aria-hidden="true"></span>
        <div class="min-w-0">
          <div class="mc-node-role">{{ node.role }}</div>
          <div class="mc-node-name">
            <span>{{ node.name }}</span>
            <span v-if="node.badge" class="mc-node-badge">{{ node.badge }}</span>
          </div>
          <template v-for="(detail, index) in node.details || []" :key="index">
            <div v-if="detail && typeof detail === 'object'" class="mc-node-detail">
              <span v-if="detail.pill" class="meta-pill" :class="detail.tone">{{ detail.text }}</span>
              <span v-else :class="detail.tone">{{ detail.text }}</span>
            </div>
            <div v-else-if="detail" class="mc-node-detail">{{ detail }}</div>
          </template>
        </div>
      </li>
    </ol>
    <div v-if="$slots.foot" class="mc-route-foot"><slot name="foot" /></div>
  </aside>
</template>
