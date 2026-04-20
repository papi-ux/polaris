<template>
  <div class="page-shell">
    <section v-if="!showEditForm" class="page-hero library-hero">
      <div class="page-hero-content">
        <div class="page-hero-copy">
          <div class="page-hero-kicker">Library Control</div>
          <h1 class="page-hero-title">{{ $t('navbar.library') }}</h1>
          <div class="page-hero-copy-inline">
            <p class="page-hero-copy-text">Curate launchable titles and keep the front row intentional.</p>
            <InfoHint size="sm" label="Library overview">
              Curate what paired clients can launch, keep the front row intentional, and pull in new titles from supported libraries without leaving the host console.
            </InfoHint>
          </div>
          <div class="page-hero-actions">
            <Button size="sm" variant="outline" @click="showImport = !showImport">
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 0 0 3 3h10a3 3 0 0 0 3-3v-1m-4-8-4-4m0 0L8 8m4-4v12"/></svg>
              {{ showImport ? 'Hide Import' : 'Import Games' }}
            </Button>
            <Button
              v-if="!listReordered"
              size="sm"
              variant="outline"
              :disabled="actionDisabled || apps.length < 2"
              @click="alphabetizeApps"
            >
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 4h13M3 8h9m-9 4h6m4 0l4-4m0 0l4 4m-4-4v12"/></svg>
              {{ $t('apps.alphabetize') }}
            </Button>
            <Button
              v-else
              size="sm"
              variant="warning"
              :disabled="actionDisabled"
              @click="saveOrder"
            >
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3m-1 4-3 3m0 0-3-3m3 3V4"/></svg>
              {{ $t('apps.save_order') }}
            </Button>
            <Button size="sm" variant="primary" :disabled="actionDisabled" @click="newApp">
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
              {{ $t('apps.add_new') }}
            </Button>
          </div>
          <div class="library-hero-meta">
            <span class="meta-pill">{{ appCount }} published</span>
            <span class="meta-pill">Host {{ hostName || 'Unknown' }}</span>
            <span class="meta-pill">{{ availableImportCount }} import ready</span>
            <span class="meta-pill">
              {{ activeApp ? `${activeApp.name} live` : 'No live app' }}
            </span>
          </div>
        </div>
      </div>
    </section>

    <section v-else class="page-header">
      <div class="page-heading">
        <div class="section-kicker">Application Editor</div>
        <h1 class="page-title">{{ editForm?.name || 'Untitled app' }}</h1>
        <p class="page-subtitle">
          Tune artwork, launch behavior, and per-title runtime overrides.
        </p>
      </div>
      <div class="page-actions">
        <div class="page-actions-secondary">
          <Button variant="outline" @click="showEditForm = false">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18 18 6M6 6l12 12"/></svg>
            {{ $t('_common.cancel') }}
          </Button>
          <Button variant="outline" :disabled="!canExportEdit" @click="exportLauncherFile(editForm)">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 0 0 3 3h10a3 3 0 0 0 3-3v-1m-4-8-4-4m0 0L8 8m4-4v12"/></svg>
            {{ $t('apps.export_launcher_file') }}
          </Button>
        </div>
        <div class="page-actions-primary">
          <Button variant="primary" :disabled="actionDisabled || !canSaveEdit" @click="save">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3m-1 4-3 3m0 0-3-3m3 3V4"/></svg>
            {{ $t('_common.save') }}
          </Button>
        </div>
      </div>
    </section>

    <div v-if="!showEditForm" class="library-main-grid">
      <section class="section-card library-workspace">
        <div class="library-surface-header">
          <div>
            <div class="section-kicker">Published Catalog</div>
            <div class="section-title-row">
              <h2 class="section-title">Catalog</h2>
              <InfoHint size="sm" label="Published catalog guidance">
                Keep the front row intentional. Reorder the titles clients see first, then open a title only when it needs artwork, command, or runtime work.
              </InfoHint>
            </div>
            <p class="section-copy max-w-3xl">Front row and launch profiles.</p>
          </div>
          <div class="library-surface-badges">
            <span class="meta-pill">{{ appCount }} published</span>
            <span v-if="currentApp" class="meta-pill border-green-400/25 bg-green-500/10 text-green-200">1 live</span>
            <span v-if="listReordered" class="meta-pill border-amber-300/25 bg-amber-300/10 text-amber-100">
              Pending order save
            </span>
          </div>
        </div>

        <div class="library-workspace-bar">
          <div class="library-inline-note">
            Drag to reorder. Save when the first rows look right.
          </div>
          <div class="library-surface-badges">
            <span class="meta-pill">Host {{ hostName || 'Unknown' }}</span>
            <span class="meta-pill">{{ availableImportCount }} import ready</span>
          </div>
        </div>

        <div v-if="visibleApps.length" class="library-list">
          <article
            v-for="(app, i) in visibleApps"
            :key="app.uuid"
            class="library-item"
            :class="app.dragover ? 'border-ice/40 bg-twilight/35 shadow-[0_12px_30px_rgba(0,0,0,0.18)]' : 'hover:border-storm/35 hover:bg-deep/45'"
            draggable="true"
            @dragstart="onDragStart($event, i)"
            @dragenter="onDragEnter($event, app)"
            @dragover="onDragOver($event)"
            @dragleave="onDragLeave(app)"
            @dragend="onDragEnd()"
            @drop="onDrop($event, app, i)"
          >
            <div class="library-item-drag" aria-hidden="true">
              <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9h8M8 15h8M5 9h.01M5 15h.01M19 9h.01M19 15h.01"/></svg>
            </div>

            <div class="library-item-body">
              <div class="library-item-media">
                <div class="relative h-20 w-14 shrink-0 overflow-hidden rounded-2xl border border-storm/20 bg-void/60 shadow-[0_14px_30px_rgba(0,0,0,0.18)]">
                  <img
                    v-if="app['image-path']"
                    :src="'./api/covers/image?name=' + encodeURIComponent(app.name)"
                    class="h-full w-full object-cover"
                    loading="lazy"
                    @error="$event.target.style.display='none'; $event.target.nextElementSibling.style.display='flex'"
                  />
                  <div class="flex h-full w-full items-center justify-center">
                    <svg class="h-5 w-5 text-storm/40" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M14.752 11.168l-3.197-2.132A1 1 0 0 0 10 9.87v4.263a1 1 0 0 0 1.555.832l3.197-2.132a1 1 0 0 0 0-1.664z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0z"/></svg>
                  </div>
                  <div v-if="currentApp === app.uuid" class="absolute inset-0 flex items-center justify-center bg-green-400/20">
                    <span class="rounded-full border border-green-400/25 bg-green-500/10 px-2 py-1 text-[10px] font-semibold uppercase tracking-[0.18em] text-green-200">
                      Live
                    </span>
                  </div>
                </div>

                <div class="min-w-0 flex-1">
                  <div class="library-item-topline">
                    <div class="min-w-0">
                      <div class="flex flex-wrap items-center gap-2">
                        <span class="meta-pill">#{{ i + 1 }}</span>
                        <h3 class="truncate text-base font-semibold text-silver">{{ app.name || 'Untitled app' }}</h3>
                        <span
                          v-if="app.source && app.source !== 'manual'"
                          class="rounded-full px-2 py-0.5 text-[10px] font-medium uppercase tracking-[0.18em]"
                          :class="sourceBadgeClass(app.source)"
                        >
                          {{ app.source }}
                        </span>
                        <span v-if="app['game-category'] && app['game-category'] !== 'unknown'" class="control-chip">
                          {{ formatCategory(app['game-category']) }}
                        </span>
                      </div>
                      <p v-if="app['cmd']" class="library-item-command">
                        {{ app['cmd'] ? trimCommand(app['cmd']) : 'No launch command saved yet.' }}
                      </p>
                    </div>
                    <span
                      class="library-state-pill"
                      :class="currentApp === app.uuid ? 'border-green-400/25 bg-green-500/10 text-green-200' : 'border-storm/20 bg-void/45 text-storm'"
                    >
                      {{ currentApp === app.uuid ? 'Live now' : 'Ready' }}
                    </span>
                  </div>

                  <div class="library-item-meta">
                    <span v-if="app.output" class="meta-pill">Output {{ app.output }}</span>
                  </div>
                </div>
              </div>

              <div v-if="app.uuid" class="library-item-actions">
                <button type="button" class="library-row-action" :disabled="actionDisabled" @click="editApp(app)">
                  Edit
                </button>
                <button
                  v-if="currentApp === app.uuid"
                  type="button"
                  class="library-row-action library-row-action-warning"
                  :disabled="actionDisabled"
                  @click="closeApp()"
                >
                  Stop
                </button>
                <a
                  v-else
                  class="library-row-action library-row-action-success no-underline"
                  @click.prevent="launchApp($event, app)"
                  :href="launchHref(app)"
                >
                  Launch
                </a>
                <button type="button" class="library-row-action library-row-action-danger" :disabled="actionDisabled" @click="showDeleteForm(app)">
                  Delete
                </button>
              </div>
            </div>
          </article>
        </div>

        <div v-else class="library-empty-state">
          <div class="mx-auto flex h-14 w-14 items-center justify-center rounded-2xl border border-storm/20 bg-void/60 text-storm">
            <svg class="h-6 w-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
          </div>
          <h3 class="mt-4 text-lg font-semibold text-silver">No applications published yet</h3>
          <p class="mx-auto mt-2 max-w-xl text-sm leading-relaxed text-storm">
            Start with one launcher or scan your existing libraries, then shape the front row after the first import pass.
          </p>
        </div>
      </section>

      <section class="section-card library-inspector">
        <div class="section-kicker">Library Inspector</div>
        <div class="section-title-row">
          <h2 class="section-title">Surface health</h2>
          <InfoHint size="sm" label="Library surface health">
            Use this rail to audit import coverage, keep titles couch-readable, and verify the host that will receive changes before you publish or export launchers.
          </InfoHint>
        </div>
        <p class="section-copy">Import coverage, catalog cues, and host context.</p>

        <div class="library-inspector-stack">
          <article class="surface-subtle library-inspector-card">
            <div class="library-inspector-title-row">
              <div class="text-sm font-semibold text-silver">Import sources</div>
              <span class="meta-pill">{{ availableImportCount }} ready</span>
            </div>
            <div class="library-inspector-list">
              <div v-for="source in importSources" :key="source.key" class="library-fact-row">
                <span class="text-sm text-silver">{{ source.label }}</span>
                <span class="meta-pill">{{ source.available }}</span>
              </div>
              <div v-if="!importSources.length" class="rounded-xl border border-storm/15 bg-void/40 px-3 py-3 text-sm text-storm">
                Run a scan to populate import sources.
              </div>
            </div>
          </article>

          <article class="surface-subtle library-inspector-card">
            <div class="text-sm font-semibold text-silver">Catalog cues</div>
            <div class="library-cue-list">
              <div class="library-fact-row">
                <span class="text-storm">Names</span>
                <span class="text-sm text-silver">Short and couch-readable</span>
              </div>
              <div class="library-fact-row">
                <span class="text-storm">Overrides</span>
                <span class="text-sm text-silver">Only where launch behavior changes</span>
              </div>
              <div class="library-fact-row">
                <span class="text-storm">Exports</span>
                <span class="text-sm text-silver">Use `.art` for frontends or automation</span>
              </div>
            </div>
          </article>

          <article class="surface-subtle library-inspector-card">
            <div class="text-sm font-semibold text-silver">Host context</div>
            <div class="library-inspector-list">
              <div class="library-fact-row">
                <span>Host name</span>
                <span class="font-medium text-silver">{{ hostName || 'Unknown host' }}</span>
              </div>
              <div class="library-fact-row">
                <span>Host UUID</span>
                <span class="font-mono text-[11px] text-silver">{{ hostUUID || 'Unavailable' }}</span>
              </div>
              <div class="library-fact-row">
                <span>Platform</span>
                <span class="font-medium capitalize text-silver">{{ platform || 'Unknown' }}</span>
              </div>
              <div class="library-fact-row">
                <span>Live app</span>
                <span class="font-medium text-silver">{{ activeApp?.name || 'None' }}</span>
              </div>
            </div>
          </article>
        </div>
      </section>
    </div>

    <section v-if="!showEditForm && showImport" class="section-card library-import-shell">
      <div class="library-surface-header">
        <div>
          <div class="section-kicker">Import Console</div>
          <div class="section-title-row">
            <h2 class="section-title">Stage new entries</h2>
            <InfoHint size="sm" label="Import console details">
              Polaris can scan supported launchers, keep already-published titles visible, and stage multiple entries before a single import pass.
            </InfoHint>
          </div>
          <p class="section-copy max-w-3xl">Scan supported libraries and pull only the titles you want on the front row.</p>
        </div>
        <div class="page-actions-secondary">
          <Button variant="outline" @click="showImport = false">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18 18 6M6 6l12 12"/></svg>
            Hide Import
          </Button>
          <Button variant="outline" :disabled="gameScanning" @click="scanGames">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 1 1-14 0 7 7 0 0 1 14 0z"/></svg>
            {{ gameScanning ? 'Scanning…' : 'Rescan Sources' }}
          </Button>
        </div>
      </div>

      <div v-if="!hasImportSources && !gameScanning" class="library-empty-state">
        <div class="mx-auto flex h-14 w-14 items-center justify-center rounded-2xl border border-storm/20 bg-void/60 text-storm">
          <svg class="h-6 w-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 1 1-14 0 7 7 0 0 1 14 0z"/></svg>
        </div>
        <h3 class="mt-4 text-lg font-semibold text-silver">Ready to scan your game libraries</h3>
        <p class="mx-auto mt-2 max-w-xl text-sm leading-relaxed text-storm">
          Start a scan to discover candidates from Steam, Lutris, and Heroic. Already-imported entries stay visible so new additions stand out.
        </p>
        <div class="mt-5">
          <Button variant="primary" :disabled="gameScanning" @click="scanGames">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 1 1-14 0 7 7 0 0 1 14 0z"/></svg>
            Scan for Games
          </Button>
        </div>
      </div>

      <div v-else-if="gameScanning" class="library-empty-state">
        <svg class="mx-auto h-7 w-7 animate-spin text-ice" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" /><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 0 1 8-8V0C5.373 0 0 5.373 0 12h4Zm2 5.291A7.962 7.962 0 0 1 4 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647Z" /></svg>
        <h3 class="mt-4 text-lg font-semibold text-silver">Scanning installed libraries</h3>
        <p class="mt-2 text-sm text-storm">Polaris is checking supported launchers and collecting import candidates.</p>
      </div>

      <div v-else class="library-import-panel">
        <div class="library-import-toolbar">
          <div class="flex flex-wrap gap-2">
            <button
              v-for="source in importSources"
              :key="source.key"
              type="button"
              class="focus-ring rounded-full border px-3 py-1.5 text-sm transition-[background-color,border-color,color] duration-200"
              :class="importTab === source.key ? source.activeClass : 'border-storm/20 bg-deep/35 text-storm hover:border-storm/40 hover:text-silver'"
              @click="importTab = source.key"
            >
              {{ source.label }} ({{ source.available }})
            </button>
          </div>
          <div class="library-surface-badges">
            <span class="meta-pill">{{ activeImportAvailableCount }} ready</span>
            <span class="meta-pill">{{ activeImportSelectedCount }} selected</span>
          </div>
        </div>

        <div class="library-workspace-bar">
          <div class="library-inline-note">
            Stage the current source, then import in one pass.
          </div>
          <div class="flex flex-wrap items-center gap-2">
            <Button variant="ghost" size="sm" @click="gameToggleAll(true, importTab)">Select All</Button>
            <Button variant="ghost" size="sm" @click="gameToggleAll(false, importTab)">Deselect All</Button>
            <Button variant="primary" :disabled="gameImporting || selectedImportCount === 0" @click="doImport">
              Import Selected
            </Button>
          </div>
        </div>

        <div class="library-import-list">
          <label
            v-for="game in activeImportGames"
            :key="game.appid || game.slug || game.name"
            class="library-import-item"
            :class="game.already_imported ? 'border-storm/20 bg-deep/20 opacity-60 cursor-default' : game.selected ? 'border-ice/40 bg-ice/5' : 'border-storm/25 bg-deep/30 hover:border-storm/45 hover:bg-deep/40'"
          >
            <div class="library-import-item-main">
              <input type="checkbox" v-model="game.selected" :disabled="game.already_imported" class="mt-1 h-4 w-4 rounded accent-ice" />
              <div class="min-w-0 flex-1">
                <div class="truncate text-sm font-medium text-silver">{{ game.name }}</div>
                <div class="library-import-item-copy">
                  {{ game.already_imported ? 'Already published in Polaris.' : 'Ready to stage for import.' }}
                </div>
              </div>
            </div>
            <div class="library-import-item-meta">
              <span v-if="game.game_category && game.game_category !== 'unknown'" class="control-chip">
                {{ formatCategory(game.game_category) }}
              </span>
              <span class="library-state-pill" :class="game.already_imported ? 'border-storm/20 bg-void/45 text-storm' : 'border-ice/30 bg-ice/10 text-ice'">
                {{ game.already_imported ? 'Imported' : 'Ready' }}
              </span>
            </div>
          </label>
        </div>
      </div>
    </section>

    <section v-if="showEditForm" class="library-editor-shell">
      <div class="library-editor-grid">
        <div class="library-editor-main">
          <article class="section-card library-editor-section library-editor-section-identity">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Identity</div>
                <div class="section-title-row">
                  <h2 class="section-title">Name and artwork</h2>
                  <InfoHint size="sm" label="Name and artwork">
                    This is the surface clients see first. Keep the title recognizable and the artwork clean before you spend time on deeper runtime tuning.
                  </InfoHint>
                </div>
              </div>
              <span class="meta-pill">{{ editForm?.uuid ? 'Published entry' : 'New entry' }}</span>
            </div>

            <div class="library-field-grid">
              <div class="library-field">
                <div class="library-label-row">
                  <label for="appName" class="library-input-label">{{ $t('apps.app_name') }}</label>
                  <InfoHint size="sm" :label="$t('apps.app_name')">{{ $t('apps.app_name_desc') }}</InfoHint>
                </div>
                <div class="library-inline-field library-inline-field-compact">
                  <input id="appName" v-model="editForm.name" type="text" class="library-input" />
                  <div class="relative" ref="coverFinderWrapper">
                    <button type="button" class="library-row-action library-cover-search-trigger" @click="showCoverFinder">
                      {{ $t('apps.find_cover') }}
                    </button>
                    <div v-if="coverFinderOpen" class="library-cover-finder-panel absolute right-0 top-full z-50 overflow-hidden rounded-xl">
                      <div class="library-cover-finder-header">
                        <h4 class="library-cover-finder-title">{{ $t('apps.covers_found') }}</h4>
                        <button type="button" class="library-cover-finder-close" @click="closeCoverFinder">
                          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
                        </button>
                      </div>
                      <div class="library-cover-finder-body" :class="{ 'opacity-50 pointer-events-none': coverFinderBusy }">
                        <div class="library-cover-finder-grid">
                          <div v-if="coverSearching" class="col-span-1">
                            <div class="cover-container flex items-center justify-center">
                              <div class="h-8 w-8 animate-spin rounded-full border-b-2 border-ice"></div>
                            </div>
                          </div>
                          <div v-for="cover in coverCandidates" :key="cover.key" class="library-cover-finder-item" @click="useCover(cover)">
                            <div class="cover-container">
                              <img class="rounded" :src="cover.url" />
                            </div>
                            <label class="library-cover-finder-label">
                              {{ cover.name }}
                            </label>
                          </div>
                        </div>
                      </div>
                    </div>
                  </div>
                </div>
              </div>

              <div class="library-field">
                <div class="library-label-row">
                  <label for="appImagePath" class="library-input-label">{{ $t('apps.image') }}</label>
                  <InfoHint size="sm" :label="$t('apps.image')">{{ $t('apps.image_desc') }}</InfoHint>
                </div>
                <input id="appImagePath" v-model="editForm['image-path']" type="text" class="library-input font-mono text-sm" />
                <div class="library-field-note">Path to local cover art or a downloaded image asset.</div>
              </div>
            </div>
          </article>

          <article class="section-card library-editor-section">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Launch Profile</div>
                <div class="section-title-row">
                  <h2 class="section-title">Start path and commands</h2>
                  <InfoHint size="sm" label="Launch profile">
                    Keep the default launch path simple. Use detached commands or launch hooks only when a title genuinely needs setup or teardown work.
                  </InfoHint>
                </div>
              </div>
            </div>

            <div class="space-y-4">
              <div class="library-field">
                <div class="library-label-row">
                  <label for="appCmd" class="library-input-label">{{ $t('apps.cmd') }}</label>
                  <InfoHint size="sm" :label="$t('apps.cmd')">
                    {{ $t('apps.cmd_desc') }} {{ $t('_common.note') }} {{ $t('apps.cmd_note') }}
                  </InfoHint>
                </div>
                <input id="appCmd" v-model="editForm.cmd" type="text" class="library-input font-mono text-sm" />
              </div>

              <div v-if="platform === 'windows'" class="library-editor-checkbox-stack">
                <Checkbox
                  class="library-checkbox"
                  id="appElevation"
                  label="_common.run_as"
                  desc="apps.run_as_desc"
                  v-model="editForm.elevated"
                  default="false"
                />
              </div>

              <div class="library-field-grid">
                <div class="library-field">
                  <div class="library-label-row">
                    <label for="appWorkingDir" class="library-input-label">{{ $t('apps.working_dir') }}</label>
                    <InfoHint size="sm" :label="$t('apps.working_dir')">{{ $t('apps.working_dir_desc') }}</InfoHint>
                  </div>
                  <input id="appWorkingDir" v-model="editForm['working-dir']" type="text" class="library-input font-mono text-sm" />
                </div>

                <div class="library-field">
                  <div class="library-label-row">
                    <label for="appOutput" class="library-input-label">{{ $t('apps.output_name') }}</label>
                    <InfoHint size="sm" :label="$t('apps.output_name')">{{ $t('apps.output_desc') }}</InfoHint>
                  </div>
                  <input id="appOutput" v-model="editForm.output" type="text" class="library-input" />
                </div>
              </div>

              <div class="library-command-card">
                <div class="library-command-card-header">
                  <div>
                    <div class="text-sm font-semibold text-silver">{{ $t('apps.detached_cmds') }}</div>
                    <div class="text-xs text-storm">Optional sidecar processes or launchers that should run outside the main app lifetime.</div>
                  </div>
                  <InfoHint size="sm" :label="$t('apps.detached_cmds')">
                    {{ $t('apps.detached_cmds_desc') }} {{ $t('_common.note') }} {{ $t('apps.detached_cmds_note') }}
                  </InfoHint>
                </div>

                <div v-if="editForm.detached.length" class="space-y-2">
                  <div v-for="(c, i) in editForm.detached" :key="`detached-${i}`" class="library-array-row">
                    <input v-model="editForm.detached[i]" type="text" class="library-input flex-1 font-mono text-sm" />
                    <button type="button" class="library-row-action library-row-action-danger" @click="editForm.detached.splice(i, 1)">
                      Remove
                    </button>
                    <button type="button" class="library-row-action" @click="editForm.detached.splice(i, 0, '')">
                      Duplicate
                    </button>
                  </div>
                </div>

                <button type="button" class="library-row-action" @click="editForm.detached.push('')">
                  + {{ $t('apps.detached_cmds_add') }}
                </button>
              </div>
            </div>
          </article>

          <article class="section-card library-editor-section">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Launch Hooks</div>
                <div class="section-title-row">
                  <h2 class="section-title">Prep and state commands</h2>
                  <InfoHint size="sm" label="Launch hooks">
                    Use these when a title needs setup before streaming starts or cleanup once the session ends. Leave them empty unless the launch path truly needs automation.
                  </InfoHint>
                </div>
              </div>
            </div>

            <div class="library-editor-checkbox-stack">
              <Checkbox
                class="library-checkbox"
                id="clientCommands"
                label="apps.allow_client_commands"
                desc="apps.allow_client_commands_desc"
                v-model="editForm['allow-client-commands']"
                default="true"
              />
            </div>

            <div class="space-y-4">
              <template v-for="type in ['prep', 'state']" :key="type">
                <div class="library-command-card">
                  <div class="library-command-card-header">
                    <div>
                      <div class="text-sm font-semibold text-silver">{{ $t('apps.cmd_' + type + '_name') }}</div>
                      <div class="text-xs text-storm">{{ $t('apps.cmd_' + type + '_desc') }}</div>
                    </div>
                  </div>

                  <Checkbox
                    class="library-checkbox"
                    :id="'excludeGlobal_' + type"
                    :label="'apps.global_' + type + '_name'"
                    :desc="'apps.global_' + type + '_desc'"
                    v-model="editForm['exclude-global-' + type + '-cmd']"
                    default="true"
                    inverse-values
                  />

                  <table v-if="editForm[type + '-cmd'].length > 0" class="library-command-table">
                    <thead>
                      <tr>
                        <th>
                          <svg class="mr-1 inline h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z"/></svg>
                          {{ $t('_common.do_cmd') }}
                        </th>
                        <th>
                          <svg class="mr-1 inline h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6"/></svg>
                          {{ $t('_common.undo_cmd') }}
                        </th>
                        <th v-if="platform === 'windows'">{{ $t('_common.run_as') }}</th>
                        <th></th>
                      </tr>
                    </thead>
                    <tbody>
                      <tr v-for="(c, i) in editForm[type + '-cmd']" :key="`${type}-${i}`">
                        <td>
                          <input v-model="c.do" type="text" class="library-input font-mono text-sm" />
                        </td>
                        <td>
                          <input v-model="c.undo" type="text" class="library-input font-mono text-sm" />
                        </td>
                        <td v-if="platform === 'windows'" class="align-middle">
                          <Checkbox :id="type + '-cmd-admin-' + i" label="_common.elevated" desc="" v-model="c.elevated" />
                        </td>
                        <td class="text-right">
                          <div class="flex justify-end gap-1">
                            <button type="button" class="library-row-action library-row-action-danger" @click="editForm[type + '-cmd'].splice(i, 1)">
                              Remove
                            </button>
                            <button type="button" class="library-row-action" @click="addCmd(editForm[type + '-cmd'], i)">
                              Insert
                            </button>
                          </div>
                        </td>
                      </tr>
                    </tbody>
                  </table>

                  <button type="button" class="library-row-action" @click="addCmd(editForm[type + '-cmd'], -1)">
                    + {{ $t('apps.add_cmds') }}
                  </button>
                </div>
              </template>
            </div>
          </article>

          <article class="section-card library-editor-section">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Environment</div>
                <div class="section-title-row">
                  <h2 class="section-title">Streaming tweaks and compatibility</h2>
                  <InfoHint size="sm" label="Environment and compatibility">
                    Keep per-title environment changes contained here so the rest of the host can stay on the global defaults.
                  </InfoHint>
                </div>
              </div>
            </div>

            <div class="space-y-4">
              <div class="library-command-card">
                <button type="button" class="flex w-full items-center justify-between text-left" @click="showTweaks = !showTweaks">
                  <div>
                    <span class="text-sm font-medium text-silver">Streaming tweaks</span>
                    <span class="ml-2 text-xs text-storm">Environment variables, Proton, MangoHud adjacents</span>
                  </div>
                  <svg class="h-4 w-4 text-storm transition-transform" :class="{ 'rotate-180': showTweaks }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
                </button>
                <div v-if="showTweaks" class="mt-3 space-y-3 border-t border-storm/20 pt-3">
                  <div class="text-xs text-storm">Set per-title environment variables like `PROTON_NO_FSYNC=1`, `DXVK_ASYNC=1`, or display-specific launch overrides.</div>
                  <div v-for="(envVar, i) in editEnvVars" :key="`env-${i}`" class="library-array-row">
                    <input type="text" v-model="envVar.key" placeholder="KEY" class="library-input w-40 font-mono text-xs" />
                    <span class="text-storm">=</span>
                    <input type="text" v-model="envVar.value" placeholder="value" class="library-input flex-1 font-mono text-xs" />
                    <button type="button" class="library-row-action library-row-action-danger" @click="editEnvVars.splice(i, 1)">
                      Remove
                    </button>
                  </div>
                  <button type="button" class="library-row-action" @click="editEnvVars.push({ key: '', value: '' })">
                    + Add Variable
                  </button>
                </div>
              </div>

              <div class="library-code-panel">
                <h4 class="mb-2 font-semibold text-silver">{{ $t('apps.env_vars_about') }}</h4>
                <div class="mb-3 text-sm text-storm">{{ $t('apps.env_vars_desc') }}</div>
                <table class="env-table text-sm">
                  <tr>
                    <td class="font-bold text-storm pr-4 py-1">{{ $t('apps.env_var_name') }}</td>
                    <td class="font-bold text-storm py-1"></td>
                  </tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_APP_ID</td><td class="text-storm py-1">{{ $t('apps.env_app_id') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_APP_NAME</td><td class="text-storm py-1">{{ $t('apps.env_app_name') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_APP_UUID</td><td class="text-storm py-1">{{ $t('apps.env_app_uuid') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_APP_STATUS</td><td class="text-storm py-1">{{ $t('apps.env_app_status') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_UUID</td><td class="text-storm py-1">{{ $t('apps.env_client_uuid') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_NAME</td><td class="text-storm py-1">{{ $t('apps.env_client_name') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_WIDTH</td><td class="text-storm py-1">{{ $t('apps.env_client_width') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_HEIGHT</td><td class="text-storm py-1">{{ $t('apps.env_client_height') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_FPS</td><td class="text-storm py-1">{{ $t('apps.env_client_fps') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_HDR</td><td class="text-storm py-1">{{ $t('apps.env_client_hdr') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_GCMAP</td><td class="text-storm py-1">{{ $t('apps.env_client_gcmap') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_HOST_AUDIO</td><td class="text-storm py-1">{{ $t('apps.env_client_host_audio') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_ENABLE_SOPS</td><td class="text-storm py-1">{{ $t('apps.env_client_enable_sops') }}</td></tr>
                  <tr><td class="font-mono text-silver pr-4 py-1">POLARIS_CLIENT_AUDIO_CONFIGURATION</td><td class="text-storm py-1">{{ $t('apps.env_client_audio_config') }}</td></tr>
                </table>

                <template v-if="platform === 'windows'">
                  <div class="mt-3 text-sm text-storm"><b>{{ $t('apps.env_rtss_cli_example') }}</b>
                    <pre class="mt-1 rounded bg-void p-2 text-silver">cmd /C \path\to\rtss-cli.exe limit:set %POLARIS_CLIENT_FPS%</pre>
                  </div>
                </template>
                <template v-if="platform === 'linux'">
                  <div class="mt-3 text-sm text-storm"><b>{{ $t('apps.env_xrandr_example') }}</b>
                    <pre class="mt-1 rounded bg-void p-2 text-silver">sh -c "xrandr --output HDMI-1 --mode \"${POLARIS_CLIENT_WIDTH}x${POLARIS_CLIENT_HEIGHT}\" --rate ${POLARIS_CLIENT_FPS}"</pre>
                  </div>
                </template>
                <template v-if="platform === 'macos'">
                  <div class="mt-3 text-sm text-storm"><b>{{ $t('apps.env_displayplacer_example') }}</b>
                    <pre class="mt-1 rounded bg-void p-2 text-silver">sh -c "displayplacer "id:&lt;screenId&gt; res:${POLARIS_CLIENT_WIDTH}x${POLARIS_CLIENT_HEIGHT} hz:${POLARIS_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre>
                  </div>
                </template>

                <div class="mt-2 text-sm">
                  <a href="https://docs.lizardbyte.dev/projects/polaris/latest/md_docs_2app__examples.html" target="_blank" class="text-ice hover:text-ice/80 no-underline">
                    {{ $t('_common.see_more') }}
                  </a>
                </div>
              </div>

              <div class="surface-muted rounded-xl border-l-4 border-blue-400 p-3 text-silver">
                <svg class="mr-1 inline h-5 w-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
                {{ $t('apps.env_sunshine_compatibility') }}
              </div>
            </div>
          </article>
        </div>

        <aside class="library-editor-rail">
          <article class="section-card library-editor-section">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Runtime</div>
                <div class="section-title-row">
                  <h2 class="section-title">Per-title behavior</h2>
                  <InfoHint size="sm" label="Per-title runtime behavior">
                    Use these only when the title needs behavior different from the host-wide defaults.
                  </InfoHint>
                </div>
              </div>
            </div>

            <div class="space-y-4">
              <div v-if="platform !== 'macos'" class="library-field">
                <div class="library-label-row">
                  <label for="gamepad" class="library-input-label">{{ $t('config.gamepad') }}</label>
                  <InfoHint size="sm" :label="$t('config.gamepad')">{{ $t('config.gamepad_desc') }}</InfoHint>
                </div>
                <select id="gamepad" v-model="editForm.gamepad" class="library-select">
                  <option value="">{{ $t('_common.default_global') }}</option>
                  <option value="disabled">{{ $t('_common.disabled') }}</option>
                  <option value="auto">{{ $t('_common.auto') }}</option>
                  <template v-if="platform === 'linux'">
                    <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
                    <option value="switch">{{ $t("config.gamepad_switch") }}</option>
                    <option value="xone">{{ $t("config.gamepad_xone") }}</option>
                  </template>
                  <template v-if="platform === 'windows'">
                    <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
                    <option value="x360">{{ $t('config.gamepad_x360') }}</option>
                  </template>
                </select>
              </div>

              <div class="library-field">
                <div class="library-label-row">
                  <label for="gameCategory" class="library-input-label">Game Category</label>
                  <InfoHint size="sm" label="Game category">
                    Classification hint for the AI optimizer. Polaris can infer this during import, but you can override it here per title.
                  </InfoHint>
                </div>
                <select id="gameCategory" v-model="editForm['game-category']" class="library-select">
                  <option value="">Not classified</option>
                  <option value="fast_action">Fast Action (FPS, Racing, Fighting)</option>
                  <option value="cinematic">Cinematic (RPG, Adventure, Strategy)</option>
                  <option value="desktop">Desktop (Productivity, Browsing)</option>
                  <option value="vr">VR (Virtual Reality)</option>
                </select>
              </div>

              <div class="library-toggle-row">
                <div>
                  <div class="library-label-row">
                    <div class="library-input-label">MangoHud Overlay</div>
                    <InfoHint size="sm" label="MangoHud overlay">
                      Show server-side GPU, CPU, thermals, and frametime data inside the stream for this title.
                    </InfoHint>
                  </div>
                  <div class="library-field-note">Per-title performance overlay.</div>
                </div>
                <label class="relative inline-flex cursor-pointer items-center">
                  <input type="checkbox" v-model="editMangoHud" class="sr-only peer" />
                  <div class="h-5 w-9 rounded-full bg-storm/40 transition-colors after:absolute after:left-[2px] after:top-[2px] after:h-4 after:w-4 after:rounded-full after:bg-white after:transition-all after:content-[''] peer-checked:bg-accent peer-checked:after:translate-x-full"></div>
                </label>
              </div>
            </div>
          </article>

          <article class="section-card library-editor-section">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Session</div>
                <div class="section-title-row">
                  <h2 class="section-title">Process behavior</h2>
                  <InfoHint size="sm" label="Process behavior">
                    These controls change how Polaris waits for, tears down, or virtualizes the app during a session.
                  </InfoHint>
                </div>
              </div>
            </div>

            <div class="library-editor-checkbox-stack">
              <Checkbox class="library-checkbox" id="autoDetach" label="apps.auto_detach" desc="apps.auto_detach_desc" v-model="editForm['auto-detach']" default="true" />
              <Checkbox class="library-checkbox" id="waitAll" label="apps.wait_all" desc="apps.wait_all_desc" v-model="editForm['wait-all']" default="true" />
              <Checkbox class="library-checkbox" id="terminateOnPause" label="apps.terminate_on_pause" desc="apps.terminate_on_pause_desc" v-model="editForm['terminate-on-pause']" default="false" />
              <Checkbox class="library-checkbox" id="virtualDisplay" label="apps.virtual_display" desc="apps.virtual_display_desc" v-model="editForm['virtual-display']" default="false" />
              <Checkbox class="library-checkbox" id="useAppIdentity" label="apps.use_app_identity" desc="apps.use_app_identity_desc" v-model="editForm['use-app-identity']" default="false" />
              <Checkbox class="library-checkbox" v-if="editForm['use-app-identity']" id="perClientAppIdentity" label="apps.per_client_app_identity" desc="apps.per_client_app_identity_desc" v-model="editForm['per-client-app-identity']" default="false" />
            </div>

            <div class="space-y-4">
              <div class="library-field">
                <div class="library-label-row">
                  <label for="exitTimeout" class="library-input-label">{{ $t('apps.exit_timeout') }}</label>
                  <InfoHint size="sm" :label="$t('apps.exit_timeout')">{{ $t('apps.exit_timeout_desc') }}</InfoHint>
                </div>
                <input id="exitTimeout" v-model="editForm['exit-timeout']" min="0" placeholder="5" type="number" class="library-input font-mono text-sm" />
              </div>

              <div v-if="platform === 'windows'" class="library-field">
                <div class="library-label-row">
                  <label for="resolutionScaleFactor" class="library-input-label">{{ $t('apps.resolution_scale_factor') }}: {{ editForm['scale-factor'] }}%</label>
                  <InfoHint size="sm" :label="$t('apps.resolution_scale_factor')">{{ $t('apps.resolution_scale_factor_desc') }}</InfoHint>
                </div>
                <input id="resolutionScaleFactor" v-model="editForm['scale-factor']" type="range" step="1" min="20" max="200" class="w-full accent-ice" />
              </div>
            </div>
          </article>

          <article class="section-card library-editor-section">
            <div class="library-editor-header">
              <div>
                <div class="section-kicker">Publish Context</div>
                <div class="section-title-row">
                  <h2 class="section-title">Host and export</h2>
                  <InfoHint size="sm" label="Host and export">
                    Saves apply to the current host immediately. Export launcher files when another frontend or automation flow needs a stable favorite entry.
                  </InfoHint>
                </div>
              </div>
            </div>

            <div class="library-inspector-list">
              <div class="library-fact-row">
                <span>Current host</span>
                <span class="font-medium text-silver">{{ hostName || 'Unknown host' }}</span>
              </div>
              <div class="library-fact-row">
                <span>Host UUID</span>
                <span class="font-mono text-[11px] text-silver">{{ hostUUID || 'Unavailable' }}</span>
              </div>
              <div class="library-fact-row">
                <span>Platform</span>
                <span class="font-medium capitalize text-silver">{{ platform || 'Unknown' }}</span>
              </div>
              <div class="library-fact-row">
                <span>Launcher export</span>
                <span class="font-medium text-silver">{{ canExportEdit ? 'Available' : 'After first save' }}</span>
              </div>
            </div>

            <div class="library-editor-actions">
              <Button v-if="canExportEdit" variant="outline" @click="exportLauncherFile(editForm)">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 0 0 3 3h10a3 3 0 0 0 3-3v-1m-4-8-4-4m0 0L8 8m4-4v12"/></svg>
                {{ $t('apps.export_launcher_file') }}
              </Button>
            </div>
          </article>
        </aside>
      </div>

      <div class="library-editor-footer">
        <Button variant="outline" @click="showEditForm = false">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
          {{ $t('_common.cancel') }}
        </Button>
        <Button variant="primary" :disabled="actionDisabled || !canSaveEdit" @click="save">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3m-1 4-3 3m0 0-3-3m3 3V4"/></svg>
          {{ $t('_common.save') }}
        </Button>
      </div>
    </section>
  </div>
</template>

<script setup>
import { computed, ref, inject } from 'vue'
import Checkbox from '../Checkbox.vue'
import Button from '../components/Button.vue'
import InfoHint from '../components/InfoHint.vue'
import { useToast } from '../composables/useToast'
import { useGameScanner } from '../composables/useGameScanner'

const { toast: showToast } = useToast()
const {
  scanning: gameScanning, importing: gameImporting,
  steamGames, lutrisGames, heroicGames,
  scan: scanGames, importSelected, toggleAll: gameToggleAll
} = useGameScanner()
const showImport = ref(false)
const importTab = ref('steam')
async function doImport() {
  const count = await importSelected()
  if (count > 0) {
    showToast(`Imported ${count} game${count > 1 ? 's' : ''}`, 'success')
    loadApps()
  }
}
const i18n = inject('i18n')

const newAppTemplate = {
  "name": "New App",
  "output": "",
  "cmd": "",
  "exclude-global-prep-cmd": false,
  "exclude-global-state-cmd": false,
  "elevated": false,
  "auto-detach": true,
  "wait-all": true,
  "exit-timeout": 5,
  "prep-cmd": [],
  "state-cmd": [],
  "detached": [],
  "image-path": "",
  "scale-factor": 100,
  "use-app-identity": false,
  "per-client-app-identity": false,
  "allow-client-commands": true,
  "virtual-display": false,
  "terminate-on-pause": false,
  "gamepad": "",
  "game-category": ""
}

// Template ref
const coverFinderWrapper = ref(null)
const showTweaks = ref(false)
const editEnvVars = ref([])
const editMangoHud = ref(false)

// Reactive state
const apps = ref([])
const showEditForm = ref(false)
const actionDisabled = ref(false)
const editForm = ref(null)
const coverSearching = ref(false)
const coverFinderBusy = ref(false)
const coverFinderOpen = ref(false)
const coverCandidates = ref([])
const platform = ref("")
const currentApp = ref("")
const draggingApp = ref(-1)
const hostName = ref("")
const hostUUID = ref("")
const listReordered = ref(false)

const visibleApps = computed(() => apps.value.filter((app) => app.uuid))
const appCount = computed(() => visibleApps.value.length)
const activeApp = computed(() => apps.value.find((app) => app.uuid === currentApp.value) || null)
const importPools = computed(() => ({
  steam: steamGames.value,
  lutris: lutrisGames.value,
  heroic: heroicGames.value,
}))
const importSources = computed(() => ([
  {
    key: 'steam',
    label: 'Steam',
    available: steamGames.value.filter((game) => !game.already_imported).length,
    activeClass: 'border-ice/30 bg-ice/10 text-ice',
  },
  {
    key: 'lutris',
    label: 'Lutris',
    available: lutrisGames.value.filter((game) => !game.already_imported).length,
    activeClass: 'border-orange-400/30 bg-orange-500/10 text-orange-300',
  },
  {
    key: 'heroic',
    label: 'Heroic',
    available: heroicGames.value.filter((game) => !game.already_imported).length,
    activeClass: 'border-purple-400/30 bg-purple-500/10 text-purple-200',
  },
]).filter((source) => importPools.value[source.key]?.length > 0))
const hasImportSources = computed(() => importSources.value.length > 0)
const allImportGames = computed(() => [...steamGames.value, ...lutrisGames.value, ...heroicGames.value])
const availableImportCount = computed(() => allImportGames.value.filter((game) => !game.already_imported).length)
const selectedImportCount = computed(() => allImportGames.value.filter((game) => game.selected && !game.already_imported).length)
const activeImportGames = computed(() => importPools.value[importTab.value] || [])
const activeImportAvailableCount = computed(() => activeImportGames.value.filter((game) => !game.already_imported).length)
const activeImportSelectedCount = computed(() => activeImportGames.value.filter((game) => game.selected && !game.already_imported).length)
const canSaveEdit = computed(() => Boolean(editForm.value?.name?.trim()))
const canExportEdit = computed(() => Boolean(editForm.value?.uuid))

function sourceBadgeClass(source) {
  return {
    steam: 'bg-blue-500/10 text-blue-300',
    lutris: 'bg-orange-500/10 text-orange-300',
    heroic: 'bg-purple-500/10 text-purple-200',
  }[source] || 'bg-void/60 text-silver'
}

function formatCategory(category) {
  return category.replaceAll('_', ' ')
}

function trimCommand(command = '') {
  if (command.length <= 44) return command
  return `${command.slice(0, 41)}...`
}

function launchHref(app) {
  return `art://launch?host_uuid=${hostUUID.value}&host_name=${hostName.value}&app_uuid=${app.uuid}&app_name=${app.name}`
}

// Methods
function onDragStart(e, idx) {
  if (showEditForm.value) {
    e.preventDefault()
    return
  }
  draggingApp.value = idx
  apps.value.push({})
}

function onDragEnter(e, app) {
  if (draggingApp.value < 0) return
  e.preventDefault()
  e.dataTransfer.dropEffect = "move"
  app.dragover = true
}

function onDragOver(e) {
  if (draggingApp.value < 0) return
  e.preventDefault()
  e.dataTransfer.dropEffect = "move"
}

function onDragLeave(app) {
  app.dragover = false
}

function onDragEnd() {
  draggingApp.value = -1
  apps.value.pop()
}

function onDrop(e, app, idx) {
  app.dragover = false
  if (draggingApp.value < 0) return
  e.preventDefault()
  if (idx === draggingApp.value || idx - 1 === draggingApp.value) return
  const draggedApp = apps.value[draggingApp.value]
  apps.value.splice(draggingApp.value, 1)
  if (idx > draggingApp.value) idx -= 1
  apps.value.splice(idx, 0, draggedApp)
  listReordered.value = true
}

function alphabetizeApps() {
  let orderStat = 0
  apps.value.sort((a, b) => {
    const result = a.name.localeCompare(b.name)
    orderStat += result
    return result
  })
  listReordered.value = orderStat !== apps.value.length - 1
  if (!listReordered.value) {
    showToast(i18n.t('apps.already_ordered'), 'info')
  }
}

function saveOrder() {
  actionDisabled.value = true
  const reorderedUUIDs = apps.value.reduce((reordered, i) => {
    if (i.uuid) reordered.push(i.uuid)
    return reordered
  }, [])

  fetch("./api/apps/reorder", {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify({ order: reorderedUUIDs })
  })
  .then(r => r.json())
  .then((r) => {
    if (!r.status) showToast(i18n.t("apps.reorder_failed") + r.error, 'error')
  })
  .finally(() => loadApps())
  .finally(() => { actionDisabled.value = false })
}

function loadApps() {
  return fetch("./api/apps", { credentials: 'include' })
  .then(r => r.json())
  .then(r => {
    apps.value = r.apps.filter(i => i.uuid).map(i => ({ ...i, launching: false, dragover: false }))
    currentApp.value = r.current_app
    hostName.value = r.host_name
    hostUUID.value = r.host_uuid
    listReordered.value = false
  })
}

function newApp() {
  editForm.value = Object.assign({}, newAppTemplate)
  editEnvVars.value = []
  editMangoHud.value = false
  showTweaks.value = false
  showEditForm.value = true
}

function launchApp(event, app) {
  const isLocalHost = ['localhost', '127.0.0.1', '[::1]'].indexOf(location.hostname) >= 0
  if (!isLocalHost && confirm(i18n.t('apps.launch_local_client'))) {
    const link = document.createElement('a')
    link.href = event.target.href
    link.click()
    return
  }
  if (confirm(i18n.t('apps.launch_warning'))) {
    actionDisabled.value = true
    fetch("./api/apps/launch", {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ uuid: app.uuid })
    })
    .then(r => r.json())
    .then(r => {
      if (!r.status) showToast(i18n.t('apps.launch_failed') + r.error, 'error')
    })
    .finally(() => {
      actionDisabled.value = false
      loadApps()
    })
  }
}

function closeApp() {
  if (confirm(i18n.t('apps.close_warning'))) {
    actionDisabled.value = true
    fetch("./api/apps/close", {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    })
    .then((r) => r.json())
    .then((r) => { if (!r.status) showToast('Failed to close app', 'error') })
    .finally(() => { actionDisabled.value = false; loadApps() })
  }
}

function editApp(app) {
  editForm.value = Object.assign({}, newAppTemplate, JSON.parse(JSON.stringify(app)))
  // Populate env vars editor from the app's env object
  const envObj = app.env || {}
  editMangoHud.value = envObj['MANGOHUD'] === '1'
  // Filter out MANGOHUD from the env vars editor (managed by toggle)
  editEnvVars.value = Object.entries(envObj)
    .filter(([key]) => key !== 'MANGOHUD')
    .map(([key, value]) => ({ key, value }))
  showTweaks.value = editEnvVars.value.length > 0
  showEditForm.value = true
}

function exportLauncherFile(app) {
  const link = document.createElement('a')
  const fileContent = `# Polaris app entry
# Exported by Polaris
# https://github.com/papi-ux/polaris

[host_uuid] ${hostUUID.value}
[host_name] ${hostName.value}
[app_uuid] ${app.uuid}
[app_name] ${app.name}
`
  link.href = `data:text/plain;charset=utf-8,${encodeURIComponent(fileContent)}`
  link.download = `${app.name}.art`
  link.click()
}

function showDeleteForm(app) {
  const resp = confirm("Are you sure to delete " + app.name + "?")
  if (resp) {
    actionDisabled.value = true
    fetch("./api/apps/delete", {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ uuid: app.uuid })
    }).then((r) => r.json())
    .then((r) => { if (!r.status) showToast("Delete failed! " + r.error, 'error') })
    .finally(() => { actionDisabled.value = false; loadApps() })
  }
}

function addCmd(cmdArr, idx) {
  const template = { do: "", undo: "" }
  if (platform.value === 'windows') template.elevated = false
  if (idx < 0) cmdArr.push(template)
  else cmdArr.splice(idx, 0, template)
}

function showCoverFinder() {
  coverCandidates.value = []
  coverSearching.value = true
  coverFinderOpen.value = true

  function getSearchBucket(name) {
    let bucket = name.substring(0, Math.min(name.length, 2)).toLowerCase().replaceAll(/[^a-z\d]/g, '')
    if (!bucket) return '@'
    return bucket
  }

  function searchCovers(name) {
    if (!name) return Promise.resolve([])
    let searchName = name.replaceAll(/\s+/g, '.').toLowerCase()
    let dbUrl = "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages"
    let bucket = getSearchBucket(name)
    return fetch(`${dbUrl}/buckets/${bucket}.json`).then(function (r) {
      if (!r.ok) throw new Error("Failed to search covers")
      return r.json()
    }).then(maps => Promise.all(Object.keys(maps).map(id => {
      let item = maps[id]
      if (item.name.replaceAll(/\s+/g, '.').toLowerCase().startsWith(searchName)) {
        return fetch(`${dbUrl}/games/${id}.json`).then(function (r) {
          return r.json()
        }).catch(() => null)
      }
      return null
    }).filter(item => item)))
      .then(results => results
        .filter(item => item && item.cover && item.cover.url)
        .map(game => {
          const thumb = game.cover.url
          const dotIndex = thumb.lastIndexOf('.')
          const slashIndex = thumb.lastIndexOf('/')
          if (dotIndex < 0 || slashIndex < 0) return null
          const slug = thumb.substring(slashIndex + 1, dotIndex)
          return {
            name: game.name,
            key: `igdb_${game.id}`,
            url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
            saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
          }
        }).filter(item => item))
  }

  searchCovers(editForm.value["name"].toString().trim())
    .then(list => coverCandidates.value = list)
    .finally(() => coverSearching.value = false)
}

function closeCoverFinder() {
  coverFinderOpen.value = false
}

function useCover(cover) {
  coverFinderBusy.value = true
  fetch("./api/covers/upload", {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify({
      key: cover.key,
      url: cover.saveUrl,
    })
  }).then(r => {
    if (!r.ok) throw new Error("Failed to download covers")
    return r.json()
  }).then(body => editForm.value["image-path"] = body.path)
    .then(() => closeCoverFinder())
    .finally(() => coverFinderBusy.value = false)
}

function save() {
  editForm.value.name = editForm.value.name.trim()
  if (!editForm.value.name) return
  editForm.value["exit-timeout"] = parseInt(editForm.value["exit-timeout"]) || 5
  editForm.value["scale-factor"] = parseInt(editForm.value["scale-factor"]) || 100
  editForm.value["image-path"] = editForm.value["image-path"].toString().trim().replace(/"/g, '')
  // Pack env vars from editor + MangoHud toggle into the env object
  const env = {}
  if (editMangoHud.value) env['MANGOHUD'] = '1'
  for (const v of editEnvVars.value) {
    if (v.key.trim()) env[v.key.trim()] = v.value
  }
  editForm.value.env = Object.keys(env).length > 0 ? env : undefined
  delete editForm.value.id
  delete editForm.value.launching
  delete editForm.value.dragover
  fetch("./api/apps", {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
    body: JSON.stringify(editForm.value),
  }).then((r) => r.json())
  .then((r) => {
    if (!r.status) {
      showToast(i18n.t('apps.save_failed') + r.error, 'error')
      throw new Error(`App save failed: ${r.error}`)
    }
  })
  .then(() => {
    showEditForm.value = false
    loadApps()
  })
}

// created() logic
loadApps()

fetch("./api/config", { credentials: 'include' })
  .then(r => r.json())
  .then(r => platform.value = r.platform)
</script>

<style scoped>
.library-hero .page-hero-content {
  display: block;
  padding: 1rem 1.15rem 1.1rem;
}

.library-hero .page-hero-title {
  margin-top: 0.4rem;
  font-size: clamp(1.7rem, 2.8vw, 2.55rem);
}

.library-hero .page-hero-copy-text {
  margin-top: 0;
  font-size: 0.8rem;
  line-height: 1.5;
}

.library-hero .page-hero-copy-inline {
  margin-top: 0.55rem;
  gap: 0.35rem;
}

.library-hero .page-hero-actions {
  margin-top: 0.85rem;
  gap: 0.45rem;
}

.library-hero-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 0.45rem;
  margin-top: 0.8rem;
}

.library-hero .meta-pill,
.library-workspace .meta-pill,
.library-inspector .meta-pill,
.library-item .meta-pill {
  font-size: 0.64rem;
  padding: 0.22rem 0.5rem;
}

.library-workspace .section-title,
.library-inspector .section-title {
  font-size: 0.96rem;
}

.library-workspace .section-copy,
.library-inspector .section-copy {
  margin-top: 0.25rem;
  font-size: 0.74rem;
  line-height: 1.45;
}

.library-main-grid {
  display: grid;
  gap: 1rem;
}

.library-surface-header,
.library-import-toolbar {
  display: flex;
  flex-direction: column;
  gap: 0.9rem;
}

.library-surface-badges,
.library-item-meta,
.library-editor-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
}

.library-workspace-bar,
.library-inline-note {
  border: 1px solid rgba(200, 214, 229, 0.09);
  background: rgba(18, 21, 34, 0.34);
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.02);
}

.library-workspace-bar {
  display: flex;
  flex-direction: column;
  gap: 0.75rem;
  margin-top: 0.8rem;
  border-radius: 1rem;
  padding: 0.65rem 0.8rem;
}

.library-inline-note {
  display: inline-flex;
  align-items: center;
  border-radius: 999px;
  padding: 0.42rem 0.8rem;
  font-size: 0.78rem;
  line-height: 1.45;
  color: rgba(232, 237, 243, 0.68);
}

.library-list,
.library-inspector-stack,
.library-import-panel,
.library-editor-main,
.library-editor-rail,
.library-editor-shell {
  display: grid;
  gap: 0.95rem;
}

.library-list {
  margin-top: 0.8rem;
}

.library-item {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  gap: 0.8rem;
  align-items: start;
  border: 1px solid rgba(200, 214, 229, 0.09);
  border-radius: 1.15rem;
  background: rgba(18, 21, 34, 0.38);
  padding: 0.8rem 0.85rem;
  transition: border-color 0.18s ease, background-color 0.18s ease, box-shadow 0.18s ease;
}

.library-item-drag {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 2.25rem;
  height: 2.25rem;
  border: 1px solid rgba(200, 214, 229, 0.09);
  border-radius: 0.85rem;
  background: rgba(10, 12, 20, 0.38);
  color: rgba(200, 214, 229, 0.56);
}

.library-item-body {
  display: flex;
  flex-direction: column;
  gap: 0.75rem;
  min-width: 0;
}

.library-item-media {
  display: flex;
  gap: 0.8rem;
  align-items: flex-start;
  min-width: 0;
}

.library-item-topline {
  display: flex;
  flex-direction: column;
  gap: 0.55rem;
}

.library-item-command {
  margin-top: 0.28rem;
  font-size: 0.74rem;
  line-height: 1.45;
  color: rgba(200, 214, 229, 0.48);
}

.library-state-pill {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 1.7rem;
  padding: 0.24rem 0.58rem;
  border: 1px solid rgba(200, 214, 229, 0.12);
  border-radius: 999px;
  font-size: 0.64rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  white-space: nowrap;
}

.library-item-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 0.4rem;
  align-items: center;
}

.library-row-action {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 1.75rem;
  padding: 0 0.7rem;
  border: 1px solid rgba(200, 214, 229, 0.12);
  border-radius: 999px;
  background: rgba(24, 28, 40, 0.42);
  color: rgba(232, 237, 243, 0.82);
  font-size: 0.68rem;
  font-weight: 700;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  transition: border-color 0.18s ease, background-color 0.18s ease, color 0.18s ease, box-shadow 0.18s ease;
  text-decoration: none;
}

.library-cover-search-trigger {
  min-height: auto;
  align-self: stretch;
  flex-shrink: 0;
  padding: 0.45rem 0.95rem;
  border-color: rgba(220, 228, 238, 0.48);
  background: rgba(30, 36, 51, 0.9);
  color: rgba(248, 250, 252, 0.98);
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.04);
}

.library-row-action:hover {
  border-color: rgba(200, 214, 229, 0.22);
  background: rgba(53, 59, 80, 0.42);
  color: rgba(245, 247, 250, 0.95);
}

.library-cover-search-trigger:hover {
  border-color: rgba(238, 243, 250, 0.62);
  background: rgba(42, 50, 69, 0.96);
  color: rgba(255, 255, 255, 1);
  box-shadow: 0 0 0 1px rgba(220, 228, 238, 0.12);
}

.library-row-action:disabled {
  cursor: not-allowed;
  opacity: 0.48;
}

.library-row-action-success:hover {
  border-color: rgba(74, 222, 128, 0.32);
  background: rgba(34, 197, 94, 0.12);
  color: rgba(187, 247, 208, 0.95);
}

.library-row-action-warning:hover {
  border-color: rgba(251, 191, 36, 0.32);
  background: rgba(251, 191, 36, 0.12);
  color: rgba(253, 230, 138, 0.95);
}

.library-row-action-danger:hover {
  border-color: rgba(248, 113, 113, 0.32);
  background: rgba(239, 68, 68, 0.12);
  color: rgba(254, 202, 202, 0.95);
}

.library-inspector-card,
.library-command-card,
.library-code-panel {
  padding: 0.85rem;
}

.library-inspector-card,
.library-command-card {
  border: 1px solid rgba(200, 214, 229, 0.09);
  border-radius: 1rem;
  background: rgba(18, 21, 34, 0.26);
  display: grid;
  gap: 0.85rem;
}

.library-inspector-title-row,
.library-editor-header,
.library-command-card-header {
  display: flex;
  flex-wrap: wrap;
  align-items: flex-start;
  justify-content: space-between;
  gap: 0.75rem;
}

.library-editor-header {
  margin-bottom: 0.8rem;
}

.library-inspector-list,
.library-cue-list {
  margin-top: 0.55rem;
  display: grid;
  gap: 0.45rem;
}

.library-fact-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  font-size: 0.76rem;
  line-height: 1.45;
  color: rgba(200, 214, 229, 0.6);
}

.library-empty-state {
  margin-top: 1rem;
  border: 1px solid rgba(200, 214, 229, 0.09);
  border-radius: 1.15rem;
  background: rgba(18, 21, 34, 0.3);
  padding: 2.4rem 1.4rem;
  text-align: center;
}

.library-import-shell {
  display: grid;
  gap: 1rem;
}

.library-import-list {
  display: grid;
  gap: 0.45rem;
  max-height: 24rem;
  overflow-y: auto;
  padding-right: 0.25rem;
}

.library-import-item {
  display: flex;
  flex-direction: column;
  gap: 0.55rem;
  border: 1px solid rgba(200, 214, 229, 0.1);
  border-radius: 1rem;
  padding: 0.7rem 0.8rem;
  transition: border-color 0.18s ease, background-color 0.18s ease, opacity 0.18s ease;
}

.library-import-item-main,
.library-array-row,
.library-inline-field,
.library-label-row {
  display: flex;
  align-items: flex-start;
  gap: 0.7rem;
}

.library-inline-field-compact {
  align-items: stretch;
}

.library-inline-field-compact .library-input {
  flex: 1 1 auto;
  min-width: 0;
}

.library-import-item-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 0.45rem;
  align-items: center;
}

.library-import-item-copy,
.library-field-note {
  font-size: 0.72rem;
  line-height: 1.45;
  color: rgba(200, 214, 229, 0.58);
}

.library-cover-finder-panel {
  width: min(24rem, calc(100vw - 3rem));
  margin-top: 0.4rem;
  border: 1px solid rgba(200, 214, 229, 0.18);
  background: rgba(10, 13, 20, 0.96);
  backdrop-filter: blur(18px);
  -webkit-backdrop-filter: blur(18px);
  box-shadow: 0 20px 48px rgba(0, 0, 0, 0.45);
}

.library-cover-finder-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  padding: 0.85rem 0.95rem;
  border-bottom: 1px solid rgba(200, 214, 229, 0.12);
}

.library-cover-finder-title {
  font-size: 0.82rem;
  font-weight: 700;
  color: rgba(245, 247, 250, 0.95);
}

.library-cover-finder-close {
  color: rgba(200, 214, 229, 0.68);
  transition: color 0.18s ease;
}

.library-cover-finder-close:hover {
  color: rgba(245, 247, 250, 0.94);
}

.library-cover-finder-body {
  max-height: 24rem;
  overflow-y: auto;
  padding: 0.85rem;
}

.library-cover-finder-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 0.75rem;
}

.library-cover-finder-item {
  cursor: pointer;
  transition: opacity 0.18s ease, transform 0.18s ease;
}

.library-cover-finder-item:hover {
  opacity: 0.88;
  transform: translateY(-1px);
}

.library-cover-finder-label {
  margin-top: 0.35rem;
  display: block;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  text-align: center;
  font-size: 0.7rem;
  color: rgba(220, 228, 238, 0.72);
}

.library-editor-grid {
  display: grid;
  gap: 1rem;
}

.library-editor-section {
  overflow: hidden;
}

.library-editor-section-identity {
  overflow: visible;
}

.library-field-grid {
  display: grid;
  gap: 1rem;
}

.library-field {
  display: grid;
  gap: 0.45rem;
}

.library-input-label {
  font-size: 0.76rem;
  font-weight: 600;
  color: rgba(232, 237, 243, 0.8);
}

.library-editor-shell .library-input,
.library-editor-shell .library-select {
  border-color: rgba(200, 214, 229, 0.18);
  background: rgba(14, 17, 26, 0.82);
  color: rgba(248, 250, 252, 0.96);
}

.library-input,
.library-select {
  width: 100%;
  border: 1px solid rgba(200, 214, 229, 0.12);
  border-radius: 0.95rem;
  background: rgba(16, 19, 29, 0.48);
  padding: 0.62rem 0.78rem;
  color: rgba(245, 247, 250, 0.92);
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.02);
  transition: border-color 0.18s ease, box-shadow 0.18s ease, background-color 0.18s ease;
}

.library-input:focus,
.library-select:focus {
  outline: none;
  border-color: rgba(200, 214, 229, 0.36);
  box-shadow: 0 0 0 1px rgba(200, 214, 229, 0.16);
}

.library-editor-shell .library-input:focus,
.library-editor-shell .library-select:focus {
  border-color: rgba(220, 228, 238, 0.44);
  background: rgba(18, 22, 33, 0.94);
  box-shadow: 0 0 0 1px rgba(220, 228, 238, 0.18), 0 0 24px rgba(200, 214, 229, 0.08);
}

.library-input::placeholder {
  color: rgba(200, 214, 229, 0.38);
}

.library-code-panel {
  border: 1px solid rgba(200, 214, 229, 0.09);
  border-radius: 1rem;
  background: rgba(26, 31, 45, 0.42);
  overflow: auto;
}

.library-toggle-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  border: 1px solid rgba(200, 214, 229, 0.09);
  border-radius: 1rem;
  background: rgba(18, 21, 34, 0.28);
  padding: 0.9rem 1rem;
}

.library-editor-checkbox-stack {
  display: grid;
  gap: 0.85rem;
}

.library-editor-checkbox-stack :deep(label.text-silver) {
  font-weight: 600;
}

.library-editor-checkbox-stack :deep(.text-sm.text-storm) {
  font-size: 0.72rem;
  line-height: 1.45;
  color: rgba(200, 214, 229, 0.56);
}

.library-command-table {
  width: 100%;
  border-collapse: separate;
  border-spacing: 0 0.55rem;
}

.library-command-table th {
  padding: 0 0 0.25rem;
  font-size: 0.68rem;
  letter-spacing: 0.05em;
  text-align: left;
  color: rgba(200, 214, 229, 0.56);
}

.library-command-table td {
  vertical-align: top;
}

.library-editor-footer {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 0.75rem;
}

.cover-container {
  padding-top: 133.33%;
  position: relative;
}

.cover-container img {
  display: block;
  position: absolute;
  top: 0;
  width: 100%;
  height: 100%;
  object-fit: cover;
  border-radius: 0.25rem;
}

.env-table td {
  padding: 0.25em;
  border-bottom: rgba(104, 123, 129, 0.25) 1px solid;
  vertical-align: top;
}

@media (min-width: 64rem) {
  .library-main-grid {
    grid-template-columns: minmax(0, 1.28fr) minmax(320px, 0.72fr);
  }

  .library-surface-header,
  .library-import-toolbar,
  .library-workspace-bar {
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
  }

  .library-field-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .library-import-item {
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
  }

  .library-item-topline {
    flex-direction: row;
    align-items: flex-start;
    justify-content: space-between;
  }
}

@media (min-width: 80rem) {
  .library-editor-grid {
    grid-template-columns: minmax(0, 1.22fr) minmax(320px, 0.78fr);
  }
}

@media (min-width: 72rem) {
  .library-item-body {
    flex-direction: row;
    align-items: center;
    justify-content: space-between;
  }

  .library-item-actions {
    justify-content: flex-end;
  }
}

@media (max-width: 47.99rem) {
  .library-item {
    grid-template-columns: 1fr;
  }

  .library-item-media,
  .library-array-row,
  .library-inline-field {
    flex-direction: column;
  }

  .library-cover-finder-panel {
    right: auto;
    left: 0;
    width: min(22rem, calc(100vw - 2.5rem));
  }

  .library-row-action {
    width: 100%;
  }

  .library-item-actions {
    width: 100%;
  }

  .library-item-actions > * {
    flex: 1 1 100%;
  }
}
</style>
