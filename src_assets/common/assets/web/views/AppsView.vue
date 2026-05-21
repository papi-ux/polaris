<template>
  <div class="page-shell operator-console library-view">
    <section v-if="!showEditForm" class="page-header">
      <div class="page-heading">
        <div class="section-title-row">
          <h1 class="page-title">{{ $t('navbar.library') }}</h1>
          <InfoHint size="sm" label="Library overview">
            Curate the apps Polaris can launch, keep the client-facing order clean, and pull in new titles from supported libraries without leaving the host console.
          </InfoHint>
        </div>
        <p class="page-subtitle">Publish, order, import, and launch apps from one surface.</p>
      </div>
      <div class="page-actions">
        <div class="page-actions-secondary">
          <Button variant="outline" @click="showImport = !showImport">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 0 0 3 3h10a3 3 0 0 0 3-3v-1m-4-8-4-4m0 0L8 8m4-4v12"/></svg>
            {{ showImport ? 'Hide Import' : 'Import Games' }}
          </Button>
          <Button
            v-if="!listReordered"
            variant="outline"
            :disabled="actionDisabled || apps.length < 2"
            @click="alphabetizeApps"
          >
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 4h13M3 8h9m-9 4h6m4 0l4-4m0 0l4 4m-4-4v12"/></svg>
            {{ $t('apps.alphabetize') }}
          </Button>
          <Button
            v-else
            variant="warning"
            :disabled="actionDisabled"
            @click="saveOrder"
          >
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3m-1 4-3 3m0 0-3-3m3 3V4"/></svg>
            {{ $t('apps.save_order') }}
          </Button>
        </div>
        <div class="page-actions-primary">
          <Button variant="primary" :disabled="actionDisabled" @click="newApp">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
            {{ $t('apps.add_new') }}
          </Button>
        </div>
      </div>
    </section>

    <section v-else class="page-header">
      <div class="page-heading">
        <div class="section-title-row">
          <h1 class="page-title">{{ editForm?.name || 'Untitled app' }}</h1>
          <InfoHint size="sm" label="Application editor overview">
            Tune launch commands, artwork, environment variables, and per-app behavior. Saving writes this launcher profile immediately.
          </InfoHint>
        </div>
        <p class="page-subtitle">Edit the launcher profile that Polaris publishes to clients.</p>
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

    <section v-if="!showEditForm && showImport" class="section-card library-import-console">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
        <div>
          <div class="section-kicker">Import Console</div>
          <div class="section-title-row">
            <h2 class="section-title">Import games</h2>
            <InfoHint size="sm" label="Import console">
              Polaris can import games from supported sources, keep track of what is already published, and stage multiple entries before one import pass.
            </InfoHint>
          </div>
        </div>
        <div class="page-actions-secondary">
          <Button variant="outline" @click="showImport = false">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18 18 6M6 6l12 12"/></svg>
            Hide Import
          </Button>
          <Button variant="outline" :disabled="gameScanning" @click="scanGames">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 1 1-14 0 7 7 0 0 1 14 0z"/></svg>
            {{ gameScanning ? 'Scanning...' : 'Rescan Sources' }}
          </Button>
        </div>
      </div>

      <div v-if="hasImportSources" class="library-import-overview">
        <div class="library-import-metric">
          <span>New</span>
          <strong>{{ availableImportCount }}</strong>
        </div>
        <div class="library-import-metric">
          <span>Staged</span>
          <strong>{{ selectedImportCount }}</strong>
        </div>
        <div class="library-import-metric">
          <span>Imported</span>
          <strong>{{ importedImportCount }}</strong>
        </div>
      </div>

      <div v-if="gameScanError" class="library-import-alert">
        <div class="min-w-0">
          <div class="text-sm font-semibold text-amber-100">Import scanner needs attention</div>
          <div class="mt-1 text-xs text-storm">{{ gameScanError }}</div>
        </div>
        <Button variant="outline" size="sm" :disabled="gameScanning" @click="scanGames">Retry scan</Button>
      </div>

      <div v-if="!hasImportSources && !gameScanning" class="mt-5 rounded-lg border border-storm/20 bg-deep/35 px-5 py-10 text-center">
        <div class="mx-auto flex h-12 w-12 items-center justify-center rounded-lg border border-storm/20 bg-void/60 text-storm">
          <svg class="h-6 w-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 1 1-14 0 7 7 0 0 1 14 0z"/></svg>
        </div>
        <div class="mt-4 flex items-center justify-center gap-2">
          <h3 class="text-lg font-semibold text-silver">Ready to scan</h3>
          <InfoHint size="sm" label="Game library scan">
            Start a scan to discover install candidates from Steam, Lutris, and Heroic. Already-imported entries stay visible so you can spot what is new.
          </InfoHint>
        </div>
        <div class="mt-5">
          <Button variant="primary" :disabled="gameScanning" @click="scanGames">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 1 1-14 0 7 7 0 0 1 14 0z"/></svg>
            Scan for Games
          </Button>
        </div>
      </div>

      <div v-else-if="gameScanning" class="mt-5 rounded-lg border border-storm/20 bg-deep/35 px-5 py-10 text-center">
        <svg class="mx-auto h-7 w-7 animate-spin text-ice" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" /><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 0 1 8-8V0C5.373 0 0 5.373 0 12h4Zm2 5.291A7.962 7.962 0 0 1 4 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647Z" /></svg>
        <h3 class="mt-4 text-lg font-semibold text-silver">Scanning installed libraries</h3>
      </div>

      <div v-else class="library-import-workspace">
        <div class="library-import-source-list">
          <button
            v-for="source in importSources"
            :key="source.key"
            type="button"
            class="focus-ring library-import-source-button"
            :class="activeImportSourceKey === source.key ? source.activeClass : 'border-storm/15 bg-deep/30 text-storm hover:border-storm/35 hover:text-silver'"
            @click="selectImportSource(source.key)"
          >
            <span class="library-import-source-head">
              <span>{{ source.label }}</span>
              <strong>{{ source.available }}</strong>
            </span>
            <span class="library-import-source-meta">
              {{ source.selected }} staged / {{ source.imported }} imported
            </span>
          </button>
        </div>

        <div class="library-import-stage">
          <div class="library-import-stage-head">
            <div class="min-w-0">
              <div class="section-kicker">Stage entries</div>
              <div class="mt-1 text-sm text-silver">
                {{ activeImportSource?.label || 'Source' }} / {{ visibleImportGames.length }} shown
              </div>
            </div>
            <Button variant="primary" :disabled="gameImporting || selectedImportCount === 0" :loading="gameImporting" @click="doImport">
              {{ importSelectedButtonLabel }}
            </Button>
          </div>

          <div class="library-import-toolbar">
            <label class="library-search">
              <svg class="h-4 w-4 shrink-0 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="m21 21-4.35-4.35M10.5 18a7.5 7.5 0 1 1 0-15 7.5 7.5 0 0 1 0 15Z"/></svg>
              <input
                v-model="importSearch"
                class="library-search-input"
                type="search"
                aria-label="Search import candidates"
                placeholder="Search candidates"
              />
            </label>
            <div class="library-filter-strip">
              <button
                v-for="filter in importStatusFilters"
                :key="filter.key"
                type="button"
                class="focus-ring library-filter-button"
                :class="{ 'is-active': importStatus === filter.key }"
                @click="importStatus = filter.key"
              >
                <span>{{ filter.label }}</span>
                <span>{{ filter.count }}</span>
              </button>
            </div>
            <div class="library-import-toolbar-actions">
              <Button variant="ghost" size="sm" :disabled="visibleImportSelectableCount === 0" @click="selectVisibleImportGames(true)">Select shown</Button>
              <Button variant="ghost" size="sm" :disabled="activeImportSelectedCount === 0" @click="clearActiveImportSource">Clear source</Button>
            </div>
          </div>

          <div v-if="visibleImportGames.length === 0" class="library-import-empty">
            <div class="text-sm font-semibold text-silver">No candidates match this view</div>
            <div class="mt-1 text-xs text-storm">Clear the search or switch filters to see more import candidates.</div>
            <Button v-if="importViewFiltered" class="mt-4" variant="outline" size="sm" @click="clearImportFilters">Clear filters</Button>
          </div>

          <div v-else class="library-import-game-grid">
            <label
              v-for="game in visibleImportGames"
              :key="game.appid || game.slug || game.name"
              class="library-import-game-card"
              :class="game.already_imported ? 'is-imported' : game.selected ? 'is-selected' : ''"
            >
              <input type="checkbox" v-model="game.selected" :disabled="game.already_imported" class="mt-1 h-4 w-4 rounded accent-ice" />
              <div class="min-w-0 flex-1">
                <div class="flex min-w-0 items-start justify-between gap-2">
                  <div class="truncate text-sm font-medium text-silver">{{ game.name }}</div>
                  <span class="control-chip">{{ game.already_imported ? 'Imported' : game.selected ? 'Staged' : 'New' }}</span>
                </div>
                <div class="mt-1 text-xs text-storm">
                  {{ game.already_imported ? 'Already in Polaris.' : game.selected ? 'Queued for import.' : 'Ready to stage.' }}
                </div>
                <div class="mt-2 flex flex-wrap gap-1.5 text-[11px] text-storm">
                  <span v-if="game.appid" class="control-chip">{{ game.appid }}</span>
                  <span v-if="game.runner" class="control-chip">{{ game.runner }}</span>
                  <span v-if="game.slug" class="control-chip">{{ game.slug }}</span>
                  <span v-if="game.game_category && game.game_category !== 'unknown'" class="control-chip">{{ formatCategory(game.game_category) }}</span>
                </div>
              </div>
            </label>
          </div>
        </div>
      </div>
    </section>

    <div v-if="!showEditForm" class="grid gap-4 xl:grid-cols-[minmax(0,1.2fr)_minmax(300px,0.8fr)]">
      <section class="section-card">
        <div class="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div>
            <div class="section-kicker">Library Surface</div>
            <div class="section-title-row">
              <h2 class="section-title">Published apps</h2>
              <InfoHint size="sm" label="Published app surface">
                Keep this list focused. Reorder entries to shape what clients see first, and open an app to manage artwork, commands, and per-title runtime tweaks.
              </InfoHint>
            </div>
          </div>
          <div class="console-inline-note">
            <span class="font-medium text-silver">Order rebuilds the client list</span>
            <InfoHint size="sm" align="right" label="Ordering note">
              Reordering can interrupt the current session because Polaris rebuilds the published app list after changes.
            </InfoHint>
          </div>
        </div>

        <div class="mt-5 grid gap-3 md:grid-cols-3">
          <article class="stat-tile">
            <div class="section-title-row">
              <div class="stat-kicker">Published Apps</div>
              <InfoHint size="sm" label="Published apps">Entries visible to paired clients and the web UI launch flow.</InfoHint>
            </div>
            <div class="stat-value">{{ appCount }}</div>
          </article>
          <article class="stat-tile">
            <div class="section-title-row">
              <div class="stat-kicker">Running Now</div>
              <InfoHint size="sm" label="Running app">
                {{ activeApp ? `${activeApp.name} is currently owning the session.` : 'No app is actively holding the current stream.' }}
              </InfoHint>
            </div>
            <div class="stat-value">{{ activeApp ? 1 : 0 }}</div>
          </article>
          <article class="stat-tile">
            <div class="section-title-row">
              <div class="stat-kicker">Import Ready</div>
              <InfoHint size="sm" label="Import candidates">Games found across Steam, Lutris, and Heroic that are not yet imported.</InfoHint>
            </div>
            <div class="stat-value">{{ availableImportCount }}</div>
          </article>
        </div>

        <div class="console-toolbar mt-5">
          <div class="library-search">
            <svg class="h-4 w-4 text-storm" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-4.35-4.35m1.85-5.15a7 7 0 1 1-14 0 7 7 0 0 1 14 0Z"/></svg>
            <input
              v-model="librarySearch"
              class="library-search-input"
              type="search"
              placeholder="Search apps, commands, sources"
              aria-label="Search library apps"
            />
          </div>
          <div class="library-filter-strip" aria-label="Library filters">
            <button
              v-for="filter in libraryFilters"
              :key="filter.key"
              type="button"
              class="focus-ring library-filter-button"
              :class="{ 'is-active': libraryFilter === filter.key }"
              @click="libraryFilter = filter.key"
            >
              <span>{{ filter.label }}</span>
              <span>{{ filter.count }}</span>
            </button>
          </div>
        </div>

        <div class="console-inline-note mt-3">
          <span class="font-medium text-silver">Ordering and export</span>
          <InfoHint size="sm" label="Ordering and export">
            Drag rows to reorder. Open an app to export its `.art` launcher file or change per-title behavior.
          </InfoHint>
          <span v-if="libraryViewFiltered" class="meta-pill border-amber-300/25 bg-amber-300/10 text-amber-100">
            Reorder disabled while filtered
          </span>
          <span v-else-if="listReordered" class="meta-pill border-amber-300/25 bg-amber-300/10 text-amber-100">
            Pending order save
          </span>
        </div>

        <div v-if="visibleApps.length" class="mt-5 space-y-3">
          <article
            v-for="app in visibleApps"
            :key="app.uuid"
            class="library-row rounded-lg border border-storm/20 bg-deep/35 p-3 transition-[border-color,background-color,box-shadow] duration-200"
            :class="app.dragover ? 'border-ice/40 bg-twilight/35 shadow-[0_12px_30px_rgba(0,0,0,0.18)]' : 'hover:border-storm/35 hover:bg-deep/45'"
            :draggable="!libraryViewFiltered"
            @dragstart="onLibraryDragStart($event, app)"
            @dragenter="onDragEnter($event, app)"
            @dragover="onDragOver($event)"
            @dragleave="onDragLeave(app)"
            @dragend="onDragEnd()"
            @drop="onLibraryDrop($event, app)"
          >
            <div class="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
              <div class="flex min-w-0 items-start gap-4">
                <div
                  class="mt-1 inline-flex h-9 w-9 shrink-0 items-center justify-center rounded-lg border border-storm/20 bg-void/45 text-storm"
                  aria-hidden="true"
                >
                  <svg class="h-4 w-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9h8M8 15h8M5 9h.01M5 15h.01M19 9h.01M19 15h.01"/></svg>
                </div>
                <div class="relative h-20 w-14 shrink-0 overflow-hidden rounded-lg border border-storm/20 bg-void/60">
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
                <div class="min-w-0">
                  <div class="flex flex-wrap items-center gap-2">
                    <h3 class="truncate text-base font-semibold text-silver">{{ app.name || 'Untitled app' }}</h3>
                    <span v-if="app.source && app.source !== 'manual'" class="rounded-full px-2 py-0.5 text-[10px] font-medium uppercase tracking-[0.18em]" :class="sourceBadgeClass(app.source)">
                      {{ app.source }}
                    </span>
                    <span v-if="app['game-category'] && app['game-category'] !== 'unknown'" class="control-chip">
                      {{ formatCategory(app['game-category']) }}
                    </span>
                  </div>
                  <div class="mt-2 text-xs text-storm">
                    {{ currentApp === app.uuid ? 'Live for connected clients.' : 'Ready.' }}
                  </div>
                  <div class="mt-3 flex flex-wrap gap-2 text-[11px] text-storm">
                    <span class="meta-pill">Position {{ appPositionLabel(app) }}</span>
                    <span v-if="app['cmd']" class="meta-pill font-mono">{{ trimCommand(app['cmd']) }}</span>
                  </div>
                </div>
              </div>

              <div v-if="app.uuid" class="library-row-actions flex shrink-0 flex-wrap items-center justify-end gap-2">
                <button class="focus-ring icon-action-button library-row-secondary-action" :disabled="actionDisabled" :aria-label="`Edit ${app.name}`" :title="`Edit ${app.name}`" @click="editApp(app)">
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2v-5m-1.414-9.414a2 2 0 1 1 2.828 2.828L11.828 15H9v-2.828l8.586-8.586z"/></svg>
                </button>
                <button class="focus-ring icon-action-button icon-action-button-danger library-row-secondary-action" :disabled="actionDisabled" :aria-label="`Delete ${app.name}`" :title="`Delete ${app.name}`" @click="showDeleteForm(app)">
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0 1 16.138 21H7.862a2 2 0 0 1-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v3M4 7h16"/></svg>
                </button>
                <button
                  v-if="currentApp === app.uuid"
                  class="focus-ring icon-action-button icon-action-button-warning"
                  :disabled="actionDisabled"
                  :aria-label="`Terminate ${app.name}`"
                  :title="`Terminate ${app.name}`"
                  @click="closeApp()"
                >
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 10a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v4a1 1 0 0 1-1 1h-4a1 1 0 0 1-1-1v-4z"/></svg>
                </button>
                <a
                  v-else
                  class="focus-ring icon-action-button icon-action-button-success library-row-launch inline-flex items-center justify-center no-underline"
                  :aria-label="`Launch ${app.name}`"
                  :title="`Launch ${app.name}`"
                  @click.prevent="launchApp($event, app)"
                  :href="launchHref(app)"
                >
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14.752 11.168l-3.197-2.132A1 1 0 0 0 10 9.87v4.263a1 1 0 0 0 1.555.832l3.197-2.132a1 1 0 0 0 0-1.664z"/></svg>
                </a>
              </div>
            </div>
          </article>
        </div>

        <div v-else-if="appCount" class="mt-5 rounded-lg border border-storm/20 bg-deep/35 px-5 py-10 text-center">
          <div class="mx-auto flex h-12 w-12 items-center justify-center rounded-lg border border-storm/20 bg-void/60 text-storm">
            <svg class="h-6 w-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-4.35-4.35m1.85-5.15a7 7 0 1 1-14 0 7 7 0 0 1 14 0Z"/></svg>
          </div>
          <div class="mt-4 flex items-center justify-center gap-2">
            <h3 class="text-lg font-semibold text-silver">No apps match this view</h3>
            <InfoHint size="sm" label="Filtered library">
              Clear the search field or switch back to All to restore the full published list.
            </InfoHint>
          </div>
          <button type="button" class="focus-ring dashboard-action-button dashboard-action-button-secondary mt-5" @click="clearLibraryFilters">
            Clear filters
          </button>
        </div>

        <div v-else class="mt-5 rounded-lg border border-storm/20 bg-deep/35 px-5 py-10 text-center">
          <div class="mx-auto flex h-12 w-12 items-center justify-center rounded-lg border border-storm/20 bg-void/60 text-storm">
            <svg class="h-6 w-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
          </div>
          <div class="mt-4 flex items-center justify-center gap-2">
            <h3 class="text-lg font-semibold text-silver">No applications published yet</h3>
            <InfoHint size="sm" label="Empty library">
              Add a launcher manually or scan your existing libraries. Polaris will publish the final list to Moonlight and Nova clients once entries are saved.
            </InfoHint>
          </div>
        </div>
      </section>

      <section class="section-card">
        <div class="section-kicker">Library Health</div>
        <div class="section-title-row">
          <h2 class="section-title">Coverage and host context</h2>
          <InfoHint size="sm" align="right" label="Library hygiene">
            Keep launcher entries short and recognizable, use per-app overrides only where needed, and export `.art` entries when you want favorite launches in frontends or automation.
          </InfoHint>
        </div>

        <div class="library-health-strip mt-4">
          <div class="library-health-chip">
            <span>Import ready</span>
            <strong>{{ availableImportCount }}</strong>
          </div>
          <div class="library-health-chip">
            <span>Platform</span>
            <strong>{{ platform || 'Unknown' }}</strong>
          </div>
          <div class="library-health-chip">
            <span>Published</span>
            <strong>{{ appCount }}</strong>
          </div>
        </div>

        <div class="mt-5 space-y-3">
          <article class="surface-subtle p-4">
            <div class="section-title-row">
              <div class="text-sm font-semibold text-silver">Import coverage</div>
              <InfoHint size="sm" label="Import coverage">Games found by supported launchers that can be imported into the published list.</InfoHint>
            </div>
            <div class="mt-3 grid gap-2 sm:grid-cols-3 xl:grid-cols-1">
              <div v-for="source in importSources" :key="source.key" class="flex items-center justify-between rounded-xl border border-storm/15 bg-void/40 px-3 py-2">
                <span class="text-sm text-silver">{{ source.label }}</span>
                <span class="meta-pill">{{ source.available }}</span>
              </div>
              <div v-if="!importSources.length" class="rounded-xl border border-storm/15 bg-void/40 px-3 py-3 text-sm text-storm">
                Scan libraries to see import candidates here.
              </div>
            </div>
          </article>

          <article class="surface-subtle p-4">
            <div class="section-title-row">
              <div class="text-sm font-semibold text-silver">Library hygiene</div>
              <InfoHint size="sm" label="Library hygiene checklist">
                Keep launcher entries short and recognizable for clients browsing from a couch or handheld. Use per-app overrides for launchers, tools, or games that need different display or command behavior. Export `.art` entries when you want to pin favorite launches into a frontend or automation flow.
              </InfoHint>
            </div>
            <div class="mt-3 text-sm text-storm">Keep the client-facing surface intentional.</div>
          </article>

          <details class="library-health-details">
            <summary class="focus-ring">Host context</summary>
            <div class="mt-3 space-y-2 text-sm text-storm">
              <div class="flex items-center justify-between gap-3">
                <span>Host name</span>
                <span class="font-medium text-silver">{{ hostName || 'Unknown host' }}</span>
              </div>
              <div class="flex items-center justify-between gap-3">
                <span>Host UUID</span>
                <span class="font-mono text-[11px] text-silver">{{ hostUUID || 'Unavailable' }}</span>
              </div>
              <div class="flex items-center justify-between gap-3">
                <span>Platform</span>
                <span class="font-medium capitalize text-silver">{{ platform || 'Unknown' }}</span>
              </div>
            </div>
          </details>
        </div>
      </section>
    </div>

    <!-- Edit form -->
    <section v-if="showEditForm" class="app-editor-layout">
      <div class="app-editor-main">
        <section class="section-card app-editor-card app-editor-section">
          <div class="app-editor-section-head">
            <div>
              <div class="section-kicker">Profile</div>
              <h2 class="section-title">Client entry</h2>
            </div>
            <span class="data-pill">{{ editForm?.uuid ? 'Published' : 'Draft' }}</span>
          </div>

          <div class="app-editor-grid">
            <div class="app-editor-field app-editor-field-wide">
              <div class="settings-field-head">
                <label for="appName" class="settings-field-label">{{ $t('apps.app_name') }}</label>
                <InfoHint size="sm" :label="$t('apps.app_name')">{{ $t('apps.app_name_desc') }}</InfoHint>
              </div>
              <div class="app-editor-inline-control">
                <input type="text" class="app-editor-input" id="appName" v-model="editForm.name" />
                <div class="relative" ref="coverFinderWrapper">
                  <button class="app-editor-secondary-button" type="button" @click="showCoverFinder">
                    {{ $t('apps.find_cover') }}
                  </button>
                  <div v-if="coverFinderOpen" class="absolute right-0 top-full mt-1 z-50 w-[min(24rem,calc(100vw-2rem))] overflow-hidden rounded-xl border border-storm bg-deep shadow-2xl">
                    <div class="flex justify-between items-center p-3 border-b border-storm">
                      <h4 class="text-silver font-medium">{{ $t('apps.covers_found') }}</h4>
                      <button type="button" class="text-storm hover:text-silver" @click="closeCoverFinder">
                        <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
                      </button>
                    </div>
                    <div class="p-3 max-h-96 overflow-y-auto" :class="{ 'opacity-50 pointer-events-none': coverFinderBusy }">
                      <div class="grid grid-cols-3 gap-3">
                        <div v-if="coverSearching" class="col-span-1">
                          <div class="cover-container flex items-center justify-center">
                            <div class="animate-spin rounded-full h-8 w-8 border-b-2 border-ice"></div>
                          </div>
                        </div>
                        <div v-for="cover in coverCandidates" :key="cover.key" class="cursor-pointer hover:opacity-80 transition" @click="useCover(cover)">
                          <div class="cover-container">
                            <img class="rounded" :src="cover.url" />
                          </div>
                          <label class="block text-xs text-center text-storm truncate mt-1">
                            {{ cover.name }}
                          </label>
                        </div>
                      </div>
                    </div>
                  </div>
                </div>
              </div>
            </div>

            <div class="app-editor-field app-editor-field-wide">
              <div class="settings-field-head">
                <label for="appImagePath" class="settings-field-label">{{ $t('apps.image') }}</label>
                <InfoHint size="sm" :label="$t('apps.image')">{{ $t('apps.image_desc') }}</InfoHint>
              </div>
              <input type="text" class="app-editor-input app-editor-input-mono" id="appImagePath" v-model="editForm['image-path']" />
            </div>

            <div class="app-editor-field">
              <div class="settings-field-head">
                <label for="gameCategory" class="settings-field-label">Game Category</label>
                <InfoHint size="sm" label="Game Category">Classification hint for Auto Quality. Auto-detected from Steam genres on import.</InfoHint>
              </div>
              <select id="gameCategory" class="app-editor-input" v-model="editForm['game-category']">
                <option value="">Not classified</option>
                <option value="fast_action">Fast Action (FPS, Racing, Fighting)</option>
                <option value="cinematic">Cinematic (RPG, Adventure, Strategy)</option>
                <option value="desktop">Desktop (Productivity, Browsing)</option>
                <option value="vr">VR (Virtual Reality)</option>
              </select>
            </div>

            <div v-if="platform !== 'macos'" class="app-editor-field">
              <div class="settings-field-head">
                <label for="gamepad" class="settings-field-label">{{ $t('config.gamepad') }}</label>
                <InfoHint size="sm" :label="$t('config.gamepad')">{{ $t('config.gamepad_desc') }}</InfoHint>
              </div>
              <select id="gamepad" class="app-editor-input" v-model="editForm.gamepad">
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
          </div>

          <div class="app-editor-toggle-row">
            <div class="section-title-row">
              <div class="text-sm font-medium text-silver">MangoHud Overlay</div>
              <InfoHint size="sm" label="MangoHud Overlay">Show GPU, CPU, temp, and frametime in the stream from the host side.</InfoHint>
            </div>
            <label class="relative inline-flex items-center cursor-pointer">
              <input type="checkbox" v-model="editMangoHud" class="sr-only peer" />
              <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
            </label>
          </div>
        </section>

        <section class="section-card app-editor-card app-editor-section">
          <div class="app-editor-section-head">
            <div>
              <div class="section-kicker">Launch</div>
              <h2 class="section-title">Command path</h2>
            </div>
            <span class="data-pill">{{ editForm.cmd?.trim() ? 'Command set' : 'Needs command' }}</span>
          </div>

          <div class="app-editor-grid">
            <div class="app-editor-field app-editor-field-wide">
              <div class="settings-field-head">
                <label for="appCmd" class="settings-field-label">{{ $t('apps.cmd') }}</label>
                <InfoHint size="sm" :label="$t('apps.cmd')">
                  {{ $t('apps.cmd_desc') }}
                  {{ $t('apps.cmd_note') }}
                </InfoHint>
              </div>
              <input type="text" class="app-editor-input app-editor-input-mono" id="appCmd" v-model="editForm.cmd" />
            </div>

            <Checkbox v-if="platform === 'windows'"
                      class="app-editor-field app-editor-field-wide"
                      id="appElevation"
                      label="_common.run_as"
                      desc="apps.run_as_desc"
                      desc-as-hint
                      v-model="editForm.elevated"
                      default="false"
            ></Checkbox>

            <div class="app-editor-field">
              <div class="settings-field-head">
                <label for="appWorkingDir" class="settings-field-label">{{ $t('apps.working_dir') }}</label>
                <InfoHint size="sm" :label="$t('apps.working_dir')">{{ $t('apps.working_dir_desc') }}</InfoHint>
              </div>
              <input type="text" class="app-editor-input app-editor-input-mono" id="appWorkingDir" v-model="editForm['working-dir']" />
            </div>

            <div class="app-editor-field">
              <div class="settings-field-head">
                <label for="appOutput" class="settings-field-label">{{ $t('apps.output_name') }}</label>
                <InfoHint size="sm" :label="$t('apps.output_name')">{{ $t('apps.output_desc') }}</InfoHint>
              </div>
              <input type="text" class="app-editor-input app-editor-input-mono" id="appOutput" v-model="editForm.output" />
            </div>
          </div>

          <details class="app-editor-inline-disclosure" :open="editForm.detached.length > 0">
            <summary class="focus-ring">
              <span>{{ $t('apps.detached_cmds') }}</span>
              <span class="control-chip">{{ editForm.detached.length }}</span>
            </summary>
            <div class="app-editor-disclosure-body">
              <div class="settings-field-head">
                <span class="settings-field-label">{{ $t('apps.detached_cmds') }}</span>
                <InfoHint size="sm" :label="$t('apps.detached_cmds')">
                  {{ $t('apps.detached_cmds_desc') }}
                  {{ $t('apps.detached_cmds_note') }}
                </InfoHint>
              </div>
              <div v-for="(c,i) in editForm.detached" :key="i" class="app-editor-command-row">
                <input type="text" v-model="editForm.detached[i]" class="app-editor-input app-editor-input-mono">
                <button class="icon-action-button icon-action-button-danger" type="button" @click="editForm.detached.splice(i,1)" :aria-label="`Remove detached command ${i + 1}`">
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
                </button>
                <button class="icon-action-button" type="button" @click="editForm.detached.splice(i, 0, '')" :aria-label="`Insert detached command before ${i + 1}`">
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
                </button>
              </div>
              <button class="app-editor-secondary-button" type="button" @click="editForm.detached.push('');">
                + {{ $t('apps.detached_cmds_add') }}
              </button>
            </div>
          </details>

          <details class="app-editor-inline-disclosure" :open="showTweaks">
            <summary class="focus-ring" @click.prevent="showTweaks = !showTweaks">
              <span>Streaming Tweaks</span>
              <span class="control-chip">{{ editEnvVars.length + (editMangoHud ? 1 : 0) }}</span>
            </summary>
            <div v-if="showTweaks" class="app-editor-disclosure-body">
              <div class="settings-field-head">
                <span class="settings-field-label">Environment variables</span>
                <InfoHint size="sm" align="right" label="Streaming Tweaks">Environment variables, Proton settings, and per-game launch-time overrides.</InfoHint>
              </div>
              <div v-for="(envVar, i) in editEnvVars" :key="i" class="app-editor-env-row">
                <input type="text" v-model="envVar.key" placeholder="KEY" class="app-editor-input app-editor-input-mono" />
                <span class="text-storm">=</span>
                <input type="text" v-model="envVar.value" placeholder="value" class="app-editor-input app-editor-input-mono" />
                <button type="button" class="icon-action-button icon-action-button-danger" @click="editEnvVars.splice(i, 1)" :aria-label="`Remove environment variable ${i + 1}`">
                  <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
                </button>
              </div>
              <button type="button" class="app-editor-secondary-button" @click="editEnvVars.push({ key: '', value: '' })">
                + Add Variable
              </button>
            </div>
          </details>
        </section>

        <section class="section-card app-editor-card app-editor-section">
          <div class="app-editor-section-head">
            <div>
              <div class="section-kicker">Automation</div>
              <h2 class="section-title">Prep and state commands</h2>
            </div>
            <span class="data-pill">{{ editForm['prep-cmd'].length + editForm['state-cmd'].length }} commands</span>
          </div>

          <Checkbox class="app-editor-toggle-card"
                    id="clientCommands"
                    label="apps.allow_client_commands"
                    desc="apps.allow_client_commands_desc"
                    desc-as-hint
                    v-model="editForm['allow-client-commands']"
                    default="true"
          ></Checkbox>

          <div class="app-editor-automation-stack">
            <details
              v-for="type in ['prep', 'state']"
              :key="type"
              class="app-editor-inline-disclosure"
              :open="editForm[type + '-cmd'].length > 0"
            >
              <summary class="focus-ring">
                <span>{{ $t('apps.cmd_' + type + '_name') }}</span>
                <span class="control-chip">{{ editForm[type + '-cmd'].length }}</span>
              </summary>
              <div class="app-editor-disclosure-body">
                <Checkbox class="app-editor-toggle-card"
                          :id="'excludeGlobal_' + type"
                          :label="'apps.global_' + type + '_name'"
                          :desc="'apps.global_' + type + '_desc'"
                          desc-as-hint
                          v-model="editForm['exclude-global-' + type + '-cmd']"
                          default="true"
                          inverse-values
                ></Checkbox>
                <div>
                  <div class="settings-field-head">
                    <span class="settings-field-label">{{ $t('apps.cmd_' + type + '_name') }}</span>
                    <InfoHint size="sm" :label="$t('apps.cmd_' + type + '_name')">{{ $t('apps.cmd_' + type + '_desc') }}</InfoHint>
                  </div>
                  <div class="app-editor-table-wrap" v-if="editForm[type + '-cmd'].length > 0">
                    <table class="app-editor-command-table">
                      <thead>
                        <tr>
                          <th>
                            <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z"/></svg>
                            {{ $t('_common.do_cmd') }}
                          </th>
                          <th>
                            <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6"/></svg>
                            {{ $t('_common.undo_cmd') }}
                          </th>
                          <th v-if="platform === 'windows'">
                            {{ $t('_common.run_as') }}
                          </th>
                          <th></th>
                        </tr>
                      </thead>
                      <tbody>
                        <tr v-for="(c, i) in editForm[type + '-cmd']" :key="i">
                          <td>
                            <input type="text" class="app-editor-input app-editor-input-mono" v-model="c.do" />
                          </td>
                          <td>
                            <input type="text" class="app-editor-input app-editor-input-mono" v-model="c.undo" />
                          </td>
                          <td v-if="platform === 'windows'" class="align-middle">
                            <Checkbox :id="type + '-cmd-admin-' + i"
                                      label="_common.elevated"
                                      desc=""
                                      v-model="c.elevated"
                            ></Checkbox>
                          </td>
                          <td>
                            <div class="flex gap-1 justify-end">
                              <button class="icon-action-button icon-action-button-danger" type="button" @click="editForm[type + '-cmd'].splice(i,1)" :aria-label="`Remove ${type} command ${i + 1}`">
                                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
                              </button>
                              <button class="icon-action-button" type="button" @click="addCmd(editForm[type + '-cmd'], i)" :aria-label="`Insert ${type} command before ${i + 1}`">
                                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
                              </button>
                            </div>
                          </td>
                        </tr>
                      </tbody>
                    </table>
                  </div>
                  <button class="app-editor-secondary-button mt-3" type="button" @click="addCmd(editForm[type + '-cmd'], -1)">
                    + {{ $t('apps.add_cmds') }}
                  </button>
                </div>
              </div>
            </details>
          </div>
        </section>

        <details class="section-card app-editor-disclosure">
          <summary class="focus-ring">
            <div>
              <div class="section-kicker">Advanced</div>
              <h2 class="section-title">Runtime behavior</h2>
            </div>
            <span class="data-pill">{{ editForm['exit-timeout'] || 5 }}s timeout</span>
          </summary>
          <div class="app-editor-disclosure-body">
            <div class="app-editor-grid">
              <Checkbox class="app-editor-toggle-card" id="autoDetach" label="apps.auto_detach" desc="apps.auto_detach_desc" desc-as-hint v-model="editForm['auto-detach']" default="true"></Checkbox>
              <Checkbox class="app-editor-toggle-card" id="waitAll" label="apps.wait_all" desc="apps.wait_all_desc" desc-as-hint v-model="editForm['wait-all']" default="true"></Checkbox>
              <Checkbox class="app-editor-toggle-card" id="terminateOnPause" label="apps.terminate_on_pause" desc="apps.terminate_on_pause_desc" desc-as-hint v-model="editForm['terminate-on-pause']" default="false"></Checkbox>
              <Checkbox class="app-editor-toggle-card" id="virtualDisplay" label="apps.virtual_display" desc="apps.virtual_display_desc" desc-as-hint v-model="editForm['virtual-display']" default="false"></Checkbox>
              <Checkbox class="app-editor-toggle-card" id="useAppIdentity" label="apps.use_app_identity" desc="apps.use_app_identity_desc" desc-as-hint v-model="editForm['use-app-identity']" default="false"></Checkbox>
              <Checkbox class="app-editor-toggle-card" v-if="editForm['use-app-identity']" id="perClientAppIdentity" label="apps.per_client_app_identity" desc="apps.per_client_app_identity_desc" desc-as-hint v-model="editForm['per-client-app-identity']" default="false"></Checkbox>
              <div class="app-editor-field">
                <div class="settings-field-head">
                  <label for="exitTimeout" class="settings-field-label">{{ $t('apps.exit_timeout') }}</label>
                  <InfoHint size="sm" :label="$t('apps.exit_timeout')">{{ $t('apps.exit_timeout_desc') }}</InfoHint>
                </div>
                <input type="number" class="app-editor-input app-editor-input-mono" id="exitTimeout" v-model="editForm['exit-timeout']" min="0" placeholder="5" />
              </div>
              <div v-if="platform === 'windows'" class="app-editor-field">
                <div class="settings-field-head">
                  <label for="resolutionScaleFactor" class="settings-field-label">{{ $t('apps.resolution_scale_factor') }}: {{ editForm['scale-factor'] }}%</label>
                  <InfoHint size="sm" :label="$t('apps.resolution_scale_factor')">{{ $t('apps.resolution_scale_factor_desc') }}</InfoHint>
                </div>
                <input type="range" step="1" min="20" max="200" class="w-full accent-ice" id="resolutionScaleFactor" v-model="editForm['scale-factor']"/>
              </div>
            </div>
          </div>
        </details>

        <details class="section-card app-editor-disclosure">
          <summary class="focus-ring">
            <div>
              <div class="section-kicker">Reference</div>
              <h2 class="section-title">{{ $t('apps.env_vars_about') }}</h2>
            </div>
            <span class="data-pill">Optional</span>
          </summary>
          <div class="app-editor-disclosure-body">
            <div class="app-editor-compat-note">
              <svg class="h-5 w-5 shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
              <span>Environment compatibility</span>
              <InfoHint size="sm" align="right" label="Environment compatibility">
                {{ $t('apps.env_sunshine_compatibility') }}
              </InfoHint>
            </div>
            <div class="app-editor-table-wrap">
              <table class="env-table text-sm">
                <tbody>
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
                </tbody>
              </table>
            </div>

            <template v-if="platform === 'windows'">
              <div class="text-sm text-storm"><b>{{ $t('apps.env_rtss_cli_example') }}</b>
                <pre class="mt-1 text-silver bg-void p-2 rounded">cmd /C \path\to\rtss-cli.exe limit:set %POLARIS_CLIENT_FPS%</pre>
              </div>
            </template>
            <template v-if="platform === 'linux'">
              <div class="text-sm text-storm"><b>{{ $t('apps.env_xrandr_example') }}</b>
                <pre class="mt-1 text-silver bg-void p-2 rounded">sh -c "xrandr --output HDMI-1 --mode \"${POLARIS_CLIENT_WIDTH}x${POLARIS_CLIENT_HEIGHT}\" --rate ${POLARIS_CLIENT_FPS}"</pre>
              </div>
            </template>
            <template v-if="platform === 'macos'">
              <div class="text-sm text-storm"><b>{{ $t('apps.env_displayplacer_example') }}</b>
                <pre class="mt-1 text-silver bg-void p-2 rounded">sh -c "displayplacer "id:&lt;screenId&gt; res:${POLARIS_CLIENT_WIDTH}x${POLARIS_CLIENT_HEIGHT} hz:${POLARIS_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre>
              </div>
            </template>

            <a href="https://docs.lizardbyte.dev/projects/polaris/latest/md_docs_2app__examples.html" target="_blank" class="text-sm text-ice hover:text-ice/80 no-underline">
              {{ $t('_common.see_more') }}
            </a>
          </div>
        </details>
      </div>

      <aside class="app-editor-rail">
        <section class="section-card app-editor-summary-card">
          <div class="section-kicker">Launcher Profile</div>
          <h2 class="section-title">{{ editForm.name || 'Untitled app' }}</h2>
          <div class="app-editor-summary-list">
            <div class="library-health-chip">
              <span>Platform</span>
              <strong>{{ platform || 'Unknown' }}</strong>
            </div>
            <div class="library-health-chip">
              <span>Category</span>
              <strong>{{ formatCategory(editForm['game-category']) || 'None' }}</strong>
            </div>
            <div class="library-health-chip">
              <span>Command</span>
              <strong>{{ editForm.cmd?.trim() ? 'Ready' : 'Missing' }}</strong>
            </div>
            <div class="library-health-chip">
              <span>Tweaks</span>
              <strong>{{ editEnvVars.length + (editMangoHud ? 1 : 0) }}</strong>
            </div>
          </div>
          <div class="app-editor-action-stack">
            <Button variant="primary" :disabled="actionDisabled || !canSaveEdit" @click="save">
              <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3m-1 4-3 3m0 0-3-3m3 3V4"/></svg>
              {{ $t('_common.save') }}
            </Button>
            <Button variant="outline" :disabled="!canExportEdit" @click="exportLauncherFile(editForm)">
              <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 0 0 3 3h10a3 3 0 0 0 3-3v-1m-4-8-4-4m0 0L8 8m4-4v12"/></svg>
              {{ $t('apps.export_launcher_file') }}
            </Button>
            <Button variant="outline" @click="showEditForm = false">
              <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18 18 6M6 6l12 12"/></svg>
              {{ $t('_common.cancel') }}
            </Button>
          </div>
        </section>
      </aside>
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
import { filterLibraryApps } from '../library-filters'
import { filterImportGames, summarizeImportGames } from '../library-imports'

const { toast: showToast } = useToast()
const {
  scanning: gameScanning, importing: gameImporting,
  steamGames, lutrisGames, heroicGames,
  error: gameScanError,
  scan: scanGames, importSelected, toggleAll: gameToggleAll
} = useGameScanner()
const showImport = ref(false)
const importTab = ref('steam')
const importSearch = ref('')
const importStatus = ref('new')
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
const librarySearch = ref("")
const libraryFilter = ref("all")

const publishedApps = computed(() => apps.value.filter((app) => app.uuid))
const visibleApps = computed(() => filterLibraryApps(apps.value, {
  query: librarySearch.value,
  filter: libraryFilter.value,
  currentApp: currentApp.value,
}))
const appCount = computed(() => publishedApps.value.length)
const activeApp = computed(() => apps.value.find((app) => app.uuid === currentApp.value) || null)
const libraryFilters = computed(() => ([
  { key: 'all', label: 'All', count: filterLibraryApps(apps.value, { filter: 'all' }).length },
  { key: 'steam', label: 'Steam', count: filterLibraryApps(apps.value, { filter: 'steam' }).length },
  { key: 'manual', label: 'Manual', count: filterLibraryApps(apps.value, { filter: 'manual' }).length },
  { key: 'fast_action', label: 'Fast Action', count: filterLibraryApps(apps.value, { filter: 'fast_action' }).length },
  { key: 'running', label: 'Running', count: filterLibraryApps(apps.value, { filter: 'running', currentApp: currentApp.value }).length },
]).filter((filter) => filter.key === 'all' || filter.count > 0))
const libraryViewFiltered = computed(() => Boolean(librarySearch.value.trim()) || libraryFilter.value !== 'all')
const importPools = computed(() => ({
  steam: steamGames.value,
  lutris: lutrisGames.value,
  heroic: heroicGames.value,
}))
const importSources = computed(() => ([
  {
    key: 'steam',
    label: 'Steam',
    ...summarizeImportGames(steamGames.value),
    activeClass: 'border-ice/30 bg-ice/10 text-ice',
  },
  {
    key: 'lutris',
    label: 'Lutris',
    ...summarizeImportGames(lutrisGames.value),
    activeClass: 'border-orange-400/30 bg-orange-500/10 text-orange-300',
  },
  {
    key: 'heroic',
    label: 'Heroic',
    ...summarizeImportGames(heroicGames.value),
    activeClass: 'border-purple-400/30 bg-purple-500/10 text-purple-200',
  },
]).filter((source) => importPools.value[source.key]?.length > 0))
const hasImportSources = computed(() => importSources.value.length > 0)
const allImportGames = computed(() => [...steamGames.value, ...lutrisGames.value, ...heroicGames.value])
const availableImportCount = computed(() => allImportGames.value.filter((game) => !game.already_imported).length)
const selectedImportCount = computed(() => allImportGames.value.filter((game) => game.selected && !game.already_imported).length)
const importedImportCount = computed(() => allImportGames.value.filter((game) => game.already_imported).length)
const activeImportSource = computed(() => importSources.value.find((source) => source.key === importTab.value) || importSources.value[0] || null)
const activeImportSourceKey = computed(() => activeImportSource.value?.key || importTab.value)
const activeImportGames = computed(() => importPools.value[activeImportSourceKey.value] || [])
const activeImportSummary = computed(() => summarizeImportGames(activeImportGames.value))
const activeImportSelectedCount = computed(() => activeImportSummary.value.selected)
const importSelectedButtonLabel = computed(() => selectedImportCount.value > 0 ? `Import ${selectedImportCount.value} Selected` : 'Import Selected')
const visibleImportGames = computed(() => filterImportGames(activeImportGames.value, {
  query: importSearch.value,
  status: importStatus.value,
}))
const visibleImportSelectableCount = computed(() => visibleImportGames.value.filter((game) => !game.already_imported).length)
const importStatusFilters = computed(() => ([
  { key: 'new', label: 'New', count: activeImportSummary.value.available },
  { key: 'selected', label: 'Selected', count: activeImportSummary.value.selected },
  { key: 'imported', label: 'Imported', count: activeImportSummary.value.imported },
  { key: 'all', label: 'All', count: activeImportSummary.value.total },
]).filter((filter) => filter.key === 'new' || filter.key === 'all' || filter.count > 0))
const importViewFiltered = computed(() => Boolean(importSearch.value.trim()) || importStatus.value !== 'new')
const canSaveEdit = computed(() => Boolean(editForm.value?.name?.trim()))
const canExportEdit = computed(() => Boolean(editForm.value?.uuid))

function sourceBadgeClass(source) {
  return {
    steam: 'bg-blue-500/10 text-blue-300',
    lutris: 'bg-orange-500/10 text-orange-300',
    heroic: 'bg-purple-500/10 text-purple-200',
  }[source] || 'bg-void/60 text-silver'
}

function formatCategory(category = '') {
  return String(category || '').replaceAll('_', ' ')
}

function trimCommand(command = '') {
  if (command.length <= 44) return command
  return `${command.slice(0, 41)}...`
}

function appOrderIndex(app) {
  return apps.value.findIndex((item) => item.uuid === app?.uuid)
}

function appPositionLabel(app) {
  const index = appOrderIndex(app)
  return index >= 0 ? index + 1 : '--'
}

function clearLibraryFilters() {
  librarySearch.value = ''
  libraryFilter.value = 'all'
}

function clearImportFilters() {
  importSearch.value = ''
  importStatus.value = 'new'
}

function selectImportSource(sourceKey) {
  importTab.value = sourceKey
  clearImportFilters()
}

function selectVisibleImportGames(selected) {
  visibleImportGames.value.forEach((game) => {
    if (!game.already_imported) game.selected = selected
  })
}

function clearActiveImportSource() {
  gameToggleAll(false, activeImportSourceKey.value)
}

function launchHref(app) {
  return `art://launch?host_uuid=${hostUUID.value}&host_name=${hostName.value}&app_uuid=${app.uuid}&app_name=${app.name}`
}

function scrollEditorToTop() {
  requestAnimationFrame(() => {
    document.querySelector('main')?.scrollTo({ top: 0, left: 0 })
  })
}

// Methods
function onLibraryDragStart(e, app) {
  if (libraryViewFiltered.value) {
    e.preventDefault()
    return
  }

  const index = appOrderIndex(app)
  if (index < 0) {
    e.preventDefault()
    return
  }

  onDragStart(e, index)
}

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

function onLibraryDrop(e, app) {
  onDrop(e, app, appOrderIndex(app))
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
  scrollEditorToTop()
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
  scrollEditorToTop()
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
</style>
