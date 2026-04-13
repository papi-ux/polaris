<template>
  <div class="my-6" v-if="!showEditForm">
      <h1 class="text-2xl font-bold text-silver">{{ $t('apps.applications_title') }}</h1>
      <div class="text-storm mt-2">{{ $t('apps.applications_desc') }}</div>
      <div class="text-storm">{{ $t('apps.applications_reorder_desc') }}</div>
      <div class="text-storm whitespace-pre-wrap">{{ $t('apps.applications_tips') }}</div>
  </div>
  <div class="my-6" v-else>
      <h1 class="text-2xl font-bold text-silver">{{ editForm.name || "&lt; NO NAME &gt;"}}</h1>
      <div class="flex gap-2 mt-2">
        <button @click="showEditForm = false" class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
          {{ $t('_common.cancel') }}
        </button>
        <button class="bg-ice text-void px-4 py-2 rounded-lg hover:bg-ice/80 transition disabled:opacity-50" :disabled="actionDisabled || !editForm.name.trim()" @click="save">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4"/></svg>
          {{ $t('_common.save') }}
        </button>
        <button class="bg-ice/20 text-ice px-4 py-2 rounded-lg hover:bg-ice/30 transition disabled:opacity-50" @click="exportLauncherFile(editForm)" :disabled="!editForm.uuid">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-8l-4-4m0 0L8 8m4-4v12"/></svg>
          {{ $t('apps.export_launcher_file') }}
        </button>
      </div>
  </div>

  <!-- Apps list -->
  <div class="card p-4" v-if="!showEditForm">
    <table class="w-full text-left">
      <thead>
        <tr class="border-b border-storm">
          <th class="py-2 text-storm text-sm">{{ $t('apps.name') }}</th>
          <th class="py-2 text-storm text-sm text-right" style="min-width: 150px">{{ $t('apps.actions') }}</th>
        </tr>
      </thead>
      <tbody>
        <tr
          v-for="(app,i) in apps"
          :key="app.uuid"
          class="border-b border-storm/30 transition-colors"
          :class="{'border-t-2 border-t-ice': app.dragover}"
          draggable="true"
          @dragstart="onDragStart($event, i)"
          @dragenter="onDragEnter($event, app)"
          @dragover="onDragOver($event)"
          @dragleave="onDragLeave(app)"
          @dragend="onDragEnd()"
          @drop="onDrop($event, app, i)"
        >
          <td class="py-2 text-silver cursor-default" @dblclick="exportLauncherFile(app)">
            <div class="flex items-center gap-3">
              <div class="w-10 h-14 rounded-md bg-void/60 shrink-0 overflow-hidden flex items-center justify-center relative">
                <img v-if="app['image-path']"
                     :src="'./api/covers/image?name=' + encodeURIComponent(app.name)"
                     class="w-full h-full object-cover"
                     loading="lazy"
                     @error="$event.target.style.display='none'; $event.target.nextElementSibling.style.display='flex'" />
                <div v-if="!app['image-path']" class="flex items-center justify-center w-full h-full">
                  <svg class="w-5 h-5 text-storm/40" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" d="M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
                </div>
                <div v-if="currentApp === app.uuid" class="absolute inset-0 bg-green-400/20 flex items-center justify-center">
                  <svg class="w-4 h-4 text-green-400" fill="currentColor" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
                </div>
              </div>
              <div class="min-w-0">
                <div class="font-medium truncate">{{ app.name || ' ' }}</div>
                <div class="flex items-center gap-1.5 mt-0.5">
                  <span v-if="app.source && app.source !== 'manual'" class="text-[10px] px-1.5 py-0.5 rounded font-medium"
                        :class="{
                          'bg-blue-500/10 text-blue-400': app.source === 'steam',
                          'bg-orange-500/10 text-orange-400': app.source === 'lutris',
                          'bg-purple-500/10 text-purple-400': app.source === 'heroic'
                        }">{{ app.source }}</span>
                  <span v-if="app['game-category'] && app['game-category'] !== 'unknown'" class="text-[10px] text-storm">{{ app['game-category'].replace('_', ' ') }}</span>
                </div>
              </div>
            </div>
          </td>
          <td v-if="app.uuid" class="py-2 text-right">
            <div class="flex gap-1 justify-end">
              <button class="bg-ice/20 text-ice px-2 py-1 rounded hover:bg-ice/30 transition disabled:opacity-50" :disabled="actionDisabled" @click="editApp(app)" title="Edit">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z"/></svg>
              </button>
              <button class="bg-red-500/20 text-red-400 px-2 py-1 rounded hover:bg-red-500/30 transition disabled:opacity-50" :disabled="actionDisabled" @click="showDeleteForm(app)" title="Delete">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
              </button>
              <button class="bg-yellow-600/20 text-yellow-500 px-2 py-1 rounded hover:bg-yellow-600/30 transition disabled:opacity-50" :disabled="actionDisabled" @click="closeApp()" v-if="currentApp === app.uuid">
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 10a1 1 0 011-1h4a1 1 0 011 1v4a1 1 0 01-1 1h-4a1 1 0 01-1-1v-4z"/></svg>
              </button>
              <a class="bg-green-600/20 text-green-400 px-2 py-1 rounded hover:bg-green-600/30 transition disabled:opacity-50 inline-flex items-center" :disabled="actionDisabled" @click.prevent="launchApp($event, app)" :href="'art://launch?host_uuid=' + hostUUID + '&host_name=' + hostName + '&app_uuid=' + app.uuid + '&app_name=' + app.name" v-else>
                <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z"/></svg>
              </a>
            </div>
          </td>
          <td v-else></td>
        </tr>
      </tbody>
    </table>
    <div class="flex justify-between items-center mt-4">
      <div class="flex gap-2">
        <button class="bg-ice text-void px-4 py-2 rounded-lg hover:bg-ice/80 transition disabled:opacity-50" @click="newApp" :disabled="actionDisabled">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
          {{ $t('apps.add_new') }}
        </button>
        <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200"
                @click="showImport = !showImport">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-8l-4-4m0 0L8 8m4-4v12"/></svg>
          Import Games
        </button>
      </div>
      <button class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200 disabled:opacity-50" @click="alphabetizeApps" :disabled="actionDisabled" v-if="!listReordered">
        <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 4h13M3 8h9m-9 4h6m4 0l4-4m0 0l4 4m-4-4v12"/></svg>
        {{ $t('apps.alphabetize') }}
      </button>
      <button class="bg-yellow-600 text-white px-4 py-2 rounded-lg hover:bg-yellow-700 transition disabled:opacity-50" @click="saveOrder" :disabled="actionDisabled" v-if="listReordered">
        <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4"/></svg>
        {{ $t('apps.save_order') }}
      </button>
    </div>

    <!-- Game Import Panel -->
    <div class="card p-5 mt-4 animate-[slide-up_0.2s_ease-out]" v-if="showImport">
      <div class="flex items-center justify-between mb-4">
        <div class="text-xs font-medium text-storm uppercase tracking-wider">Import Games</div>
        <button @click="showImport = false" class="text-storm hover:text-ice transition-colors">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
        </button>
      </div>

      <!-- Scan button (before any results) -->
      <div v-if="!steamGames.length && !lutrisGames.length && !heroicGames.length && !gameScanning" class="text-center py-6">
        <p class="text-storm mb-4">Scan your system for installed games</p>
        <button @click="scanGames" class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z"/></svg>
          Scan for Games
        </button>
      </div>
      <div v-if="gameScanning" class="text-center py-6">
        <svg class="w-6 h-6 animate-spin text-ice mx-auto mb-2" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" /><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z" /></svg>
        <p class="text-storm">Scanning...</p>
      </div>

      <!-- Source tabs -->
      <div v-if="steamGames.length || lutrisGames.length || heroicGames.length" class="space-y-3">
        <div class="flex gap-1 border-b border-storm/20 mb-3">
          <button v-if="steamGames.length" @click="importTab = 'steam'"
                  class="px-3 py-1.5 text-xs font-medium rounded-t-lg transition-colors"
                  :class="importTab === 'steam' ? 'bg-ice/10 text-ice border-b-2 border-ice' : 'text-storm hover:text-silver'">
            Steam ({{ steamGames.filter(g => !g.already_imported).length }})
          </button>
          <button v-if="lutrisGames.length" @click="importTab = 'lutris'"
                  class="px-3 py-1.5 text-xs font-medium rounded-t-lg transition-colors"
                  :class="importTab === 'lutris' ? 'bg-orange-500/10 text-orange-400 border-b-2 border-orange-400' : 'text-storm hover:text-silver'">
            Lutris ({{ lutrisGames.filter(g => !g.already_imported).length }})
          </button>
          <button v-if="heroicGames.length" @click="importTab = 'heroic'"
                  class="px-3 py-1.5 text-xs font-medium rounded-t-lg transition-colors"
                  :class="importTab === 'heroic' ? 'bg-purple-500/10 text-purple-400 border-b-2 border-purple-400' : 'text-storm hover:text-silver'">
            Heroic ({{ heroicGames.filter(g => !g.already_imported).length }})
          </button>
        </div>

        <!-- Game list for active tab -->
        <template v-for="(tabGames, tabKey) in { steam: steamGames, lutris: lutrisGames, heroic: heroicGames }" :key="tabKey">
          <div v-if="importTab === tabKey && tabGames.length">
            <div class="flex items-center justify-between mb-2">
              <div class="text-sm text-silver">{{ tabGames.filter(g => !g.already_imported).length }} available · {{ tabGames.filter(g => g.selected && !g.already_imported).length }} selected</div>
              <div class="flex gap-2">
                <button @click="gameToggleAll(true, tabKey)" class="text-xs text-storm hover:text-ice transition-colors">Select All</button>
                <button @click="gameToggleAll(false, tabKey)" class="text-xs text-storm hover:text-ice transition-colors">Deselect All</button>
              </div>
            </div>
            <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-2 max-h-80 overflow-y-auto pr-1">
              <label v-for="game in tabGames" :key="game.appid || game.slug || game.name"
                     class="flex items-center gap-3 p-2.5 rounded-lg border transition-all duration-150 cursor-pointer"
                     :class="game.already_imported ? 'border-storm/20 opacity-50 cursor-default' : game.selected ? 'border-ice/40 bg-ice/5' : 'border-storm/30 hover:border-storm'">
                <input type="checkbox" v-model="game.selected" :disabled="game.already_imported" class="w-4 h-4 rounded accent-ice" />
                <div class="flex-1 min-w-0">
                  <div class="text-sm text-silver truncate">{{ game.name }}</div>
                  <div class="text-xs text-storm" v-if="game.already_imported">Already imported</div>
                  <div class="flex flex-wrap gap-1 mt-0.5" v-if="game.game_category && game.game_category !== 'unknown'">
                    <span class="inline-block px-1.5 py-0.5 rounded text-[10px] font-medium"
                          :class="{
                            'bg-red-500/10 text-red-400': game.game_category === 'fast_action',
                            'bg-purple-500/10 text-purple-400': game.game_category === 'cinematic',
                            'bg-blue-500/10 text-blue-400': game.game_category === 'desktop',
                            'bg-green-500/10 text-green-400': game.game_category === 'vr'
                          }">{{ game.game_category.replace('_', ' ') }}</span>
                  </div>
                </div>
              </label>
            </div>
          </div>
        </template>

        <div class="flex items-center justify-between pt-3 border-t border-storm/20">
          <button @click="scanGames" class="text-xs text-storm hover:text-ice transition-colors">Rescan</button>
          <button @click="doImport" :disabled="gameImporting || [...steamGames, ...lutrisGames, ...heroicGames].filter(g => g.selected && !g.already_imported).length === 0"
                  class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg bg-ice text-void hover:bg-ice/90 hover:shadow-[0_0_24px_rgba(200,214,229,0.2)] transition-all duration-200 disabled:opacity-50">
            Import Selected
          </button>
        </div>
      </div>
    </div>
  </div>

  <!-- Edit form -->
  <div class="card mt-2" v-else>
    <div class="p-6 space-y-4">
      <!-- Application Name -->
      <div>
        <label for="appName" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.app_name') }}</label>
        <div class="flex gap-2">
          <input type="text" class="flex-1 bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" id="appName" v-model="editForm.name" />
          <div class="relative" ref="coverFinderWrapper">
            <button class="bg-storm/30 text-silver px-3 py-2 rounded-lg hover:bg-storm/50 transition text-sm whitespace-nowrap" type="button"
              @click="showCoverFinder">
              {{ $t('apps.find_cover') }} (Online)
            </button>
            <div v-if="coverFinderOpen" class="absolute right-0 top-full mt-1 z-50 w-96 bg-deep border border-storm rounded-xl shadow-2xl overflow-hidden">
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
                  <div v-for="(cover,i) in coverCandidates" :key="'i'" class="cursor-pointer hover:opacity-80 transition"
                    @click="useCover(cover)">
                    <div class="cover-container">
                      <img class="rounded" :src="cover.url" />
                    </div>
                    <label class="block text-xs text-center text-storm truncate mt-1">
                      {{cover.name}}
                    </label>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>
        <div class="text-sm text-storm mt-1">{{ $t('apps.app_name_desc') }}</div>
      </div>

      <!-- Application Image -->
      <div>
        <label for="appImagePath" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.image') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" id="appImagePath"
          v-model="editForm['image-path']" />
        <div class="text-sm text-storm mt-1">{{ $t('apps.image_desc') }}</div>
      </div>

      <!-- gamepad override -->
      <div v-if="platform !== 'macos'">
        <label for="gamepad" class="block text-sm font-medium text-storm mb-1">{{ $t('config.gamepad') }}</label>
        <select id="gamepad" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="editForm.gamepad">
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
        <div class="text-sm text-storm mt-1">{{ $t('config.gamepad_desc') }}</div>
      </div>

      <!-- game category -->
      <div>
        <label for="gameCategory" class="block text-sm font-medium text-storm mb-1">Game Category</label>
        <select id="gameCategory" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none" v-model="editForm['game-category']">
          <option value="">Not classified</option>
          <option value="fast_action">Fast Action (FPS, Racing, Fighting)</option>
          <option value="cinematic">Cinematic (RPG, Adventure, Strategy)</option>
          <option value="desktop">Desktop (Productivity, Browsing)</option>
          <option value="vr">VR (Virtual Reality)</option>
        </select>
        <div class="text-sm text-storm mt-1">Classification hint for AI optimizer. Auto-detected from Steam genres on import.</div>
      </div>

      <!-- MangoHud toggle -->
      <div class="flex items-center justify-between p-3 border border-storm/30 rounded-lg">
        <div>
          <div class="text-sm font-medium text-silver">MangoHud Overlay</div>
          <div class="text-xs text-storm">Show GPU, CPU, temp, frametime in the stream (server-side)</div>
        </div>
        <label class="relative inline-flex items-center cursor-pointer">
          <input type="checkbox" v-model="editMangoHud" class="sr-only peer" />
          <div class="w-9 h-5 bg-storm/40 peer-focus:outline-none rounded-full peer peer-checked:bg-accent transition-colors after:content-[''] after:absolute after:top-[2px] after:left-[2px] after:bg-white after:rounded-full after:h-4 after:w-4 after:transition-all peer-checked:after:translate-x-full"></div>
        </label>
      </div>

      <!-- Streaming Tweaks (per-game environment variables) -->
      <div class="border border-storm/30 rounded-lg overflow-hidden">
        <button type="button" class="w-full flex items-center justify-between p-3 text-left hover:bg-ice/5 transition-colors"
                @click="showTweaks = !showTweaks">
          <div>
            <span class="text-sm font-medium text-silver">Streaming Tweaks</span>
            <span class="text-xs text-storm ml-2">Environment variables, Proton settings</span>
          </div>
          <svg class="w-4 h-4 text-storm transition-transform" :class="{ 'rotate-180': showTweaks }" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
        </button>
        <div v-if="showTweaks" class="p-3 pt-0 space-y-3 border-t border-storm/20">
          <div class="text-xs text-storm">Set per-game environment variables (e.g. PROTON_NO_FSYNC=1, DXVK_ASYNC=1, MANGOHUD=1)</div>
          <div v-for="(envVar, i) in editEnvVars" :key="i" class="flex items-center gap-2">
            <input type="text" v-model="envVar.key" placeholder="KEY" class="w-40 bg-deep border border-storm rounded-lg px-3 py-1.5 text-silver focus:border-ice focus:outline-none font-mono text-xs" />
            <span class="text-storm">=</span>
            <input type="text" v-model="envVar.value" placeholder="value" class="flex-1 bg-deep border border-storm rounded-lg px-3 py-1.5 text-silver focus:border-ice focus:outline-none font-mono text-xs" />
            <button type="button" class="bg-red-500/20 text-red-400 px-2 py-1 rounded hover:bg-red-500/30 transition" @click="editEnvVars.splice(i, 1)">
              <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
            </button>
          </div>
          <button type="button" class="bg-ice/20 text-ice px-3 py-1 rounded-lg hover:bg-ice/30 transition text-xs" @click="editEnvVars.push({ key: '', value: '' })">
            + Add Variable
          </button>
        </div>
      </div>

      <!-- command -->
      <div>
        <label for="appCmd" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.cmd') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" id="appCmd"
          v-model="editForm.cmd" />
        <div class="text-sm text-storm mt-1">
          {{ $t('apps.cmd_desc') }}<br>
          <b>{{ $t('_common.note') }}</b> {{ $t('apps.cmd_note') }}
        </div>
      </div>

      <!-- elevation -->
      <Checkbox v-if="platform === 'windows'"
                class="mb-3"
                id="appElevation"
                label="_common.run_as"
                desc="apps.run_as_desc"
                v-model="editForm.elevated"
                default="false"
      ></Checkbox>

      <!-- detached -->
      <div>
        <label class="block text-sm font-medium text-storm mb-1">{{ $t('apps.detached_cmds') }}</label>
        <div v-for="(c,i) in editForm.detached" class="flex items-center gap-2 my-2">
          <input type="text" v-model="editForm.detached[i]" class="flex-1 bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm">
          <button class="bg-red-500 text-white px-2 py-1 rounded hover:bg-red-600 transition" @click="editForm.detached.splice(i,1)">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
          </button>
          <button class="bg-ice/20 text-ice px-2 py-1 rounded hover:bg-ice/30 transition" @click="editForm.detached.splice(i, 0, '')">
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
          </button>
        </div>
        <button class="bg-ice/20 text-ice px-3 py-1 rounded-lg hover:bg-ice/30 transition text-sm" @click="editForm.detached.push('');">
          + {{ $t('apps.detached_cmds_add') }}
        </button>
        <div class="text-sm text-storm mt-1">
          {{ $t('apps.detached_cmds_desc') }}<br>
          <b>{{ $t('_common.note') }}</b> {{ $t('apps.detached_cmds_note') }}
        </div>
      </div>

      <!-- allow client commands -->
      <Checkbox class="mb-3"
                id="clientCommands"
                label="apps.allow_client_commands"
                desc="apps.allow_client_commands_desc"
                v-model="editForm['allow-client-commands']"
                default="true"
      ></Checkbox>

      <!-- prep and state-cmd -->
      <template v-for="type in ['prep', 'state']">
        <Checkbox class="mb-3"
                  :id="'excludeGlobal_' + type"
                  :label="'apps.global_' + type + '_name'"
                  :desc="'apps.global_' + type + '_desc'"
                  v-model="editForm['exclude-global-' + type + '-cmd']"
                  default="true"
                  inverse-values
        ></Checkbox>
        <div>
          <label class="block text-sm font-medium text-storm mb-1">{{ $t('apps.cmd_' + type + '_name') }}</label>
          <div class="text-sm text-storm whitespace-pre-wrap">{{ $t('apps.cmd_' + type + '_desc') }}</div>
          <table class="w-full text-left mt-2" v-if="editForm[type + '-cmd'].length > 0">
            <thead>
              <tr class="border-b border-storm">
                <th class="py-2 text-storm text-sm">
                  <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z"/></svg>
                  {{ $t('_common.do_cmd') }}
                </th>
                <th class="py-2 text-storm text-sm">
                  <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6"/></svg>
                  {{ $t('_common.undo_cmd') }}
                </th>
                <th class="py-2 text-storm text-sm" v-if="platform === 'windows'">
                  {{ $t('_common.run_as') }}
                </th>
                <th class="py-2"></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="(c, i) in editForm[type + '-cmd']" class="border-b border-storm/30">
                <td class="py-2 pr-2">
                  <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="c.do" />
                </td>
                <td class="py-2 pr-2">
                  <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" v-model="c.undo" />
                </td>
                <td v-if="platform === 'windows'" class="py-2 pr-2 align-middle">
                  <Checkbox :id="type + '-cmd-admin-' + i"
                            label="_common.elevated"
                            desc=""
                            v-model="c.elevated"
                  ></Checkbox>
                </td>
                <td class="py-2 text-right">
                  <div class="flex gap-1 justify-end">
                    <button class="bg-red-500 text-white px-2 py-1 rounded hover:bg-red-600 transition" @click="editForm[type + '-cmd'].splice(i,1)">
                      <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16"/></svg>
                    </button>
                    <button class="bg-ice/20 text-ice px-2 py-1 rounded hover:bg-ice/30 transition" @click="addCmd(editForm[type + '-cmd'], i)">
                      <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4"/></svg>
                    </button>
                  </div>
                </td>
              </tr>
            </tbody>
          </table>
          <div class="mt-2 mb-4">
            <button class="bg-ice/20 text-ice px-3 py-1 rounded-lg hover:bg-ice/30 transition text-sm" @click="addCmd(editForm[type + '-cmd'], -1)">
              + {{ $t('apps.add_cmds') }}
            </button>
          </div>
        </div>
      </template>

      <!-- working dir -->
      <div>
        <label for="appWorkingDir" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.working_dir') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" id="appWorkingDir"
          v-model="editForm['working-dir']" />
        <div class="text-sm text-storm mt-1">{{ $t('apps.working_dir_desc') }}</div>
      </div>

      <!-- output -->
      <div>
        <label for="appOutput" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.output_name') }}</label>
        <input type="text" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" id="appOutput"
          v-model="editForm.output" />
        <div class="text-sm text-storm mt-1">{{ $t('apps.output_desc') }}</div>
      </div>

      <!-- auto-detach -->
      <Checkbox class="mb-3" id="autoDetach" label="apps.auto_detach" desc="apps.auto_detach_desc" v-model="editForm['auto-detach']" default="true"></Checkbox>
      <!-- wait for all processes -->
      <Checkbox class="mb-3" id="waitAll" label="apps.wait_all" desc="apps.wait_all_desc" v-model="editForm['wait-all']" default="true"></Checkbox>
      <!-- terminate on pause -->
      <Checkbox class="mb-3" id="terminateOnPause" label="apps.terminate_on_pause" desc="apps.terminate_on_pause_desc" v-model="editForm['terminate-on-pause']" default="false"></Checkbox>

      <!-- exit timeout -->
      <div>
        <label for="exitTimeout" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.exit_timeout') }}</label>
        <input type="number" class="w-full bg-deep border border-storm rounded-lg px-3 py-2 text-silver focus:border-ice focus:outline-none font-mono text-sm" id="exitTimeout"
               v-model="editForm['exit-timeout']" min="0" placeholder="5" />
        <div class="text-sm text-storm mt-1">{{ $t('apps.exit_timeout_desc') }}</div>
      </div>

      <!-- use virtual display -->
      <Checkbox class="mb-3" id="virtualDisplay" label="apps.virtual_display" desc="apps.virtual_display_desc" v-model="editForm['virtual-display']" default="false"></Checkbox>
      <!-- use app identity -->
      <Checkbox class="mb-3" id="useAppIdentity" label="apps.use_app_identity" desc="apps.use_app_identity_desc" v-model="editForm['use-app-identity']" default="false"></Checkbox>
      <!-- per-client app identity -->
      <Checkbox class="mb-3" v-if="editForm['use-app-identity']" id="perClientAppIdentity" label="apps.per_client_app_identity" desc="apps.per_client_app_identity_desc" v-model="editForm['per-client-app-identity']" default="false"></Checkbox>

      <!-- resolution scale factor -->
      <div v-if="platform === 'windows'">
        <label for="resolutionScaleFactor" class="block text-sm font-medium text-storm mb-1">{{ $t('apps.resolution_scale_factor') }}: {{editForm['scale-factor']}}%</label>
        <input type="range" step="1" min="20" max="200" class="w-full accent-ice" id="resolutionScaleFactor" v-model="editForm['scale-factor']"/>
        <div class="text-sm text-storm mt-1">{{ $t('apps.resolution_scale_factor_desc') }}</div>
      </div>

      <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-4 rounded-lg overflow-auto">
        <h4 class="text-silver font-semibold mb-2">{{ $t('apps.env_vars_about') }}</h4>
        <div class="text-sm text-storm mb-3">{{ $t('apps.env_vars_desc') }}</div>
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
          <div class="text-sm text-storm mt-3"><b>{{ $t('apps.env_rtss_cli_example') }}</b>
            <pre class="mt-1 text-silver bg-void p-2 rounded">cmd /C \path\to\rtss-cli.exe limit:set %POLARIS_CLIENT_FPS%</pre>
          </div>
        </template>
        <template v-if="platform === 'linux'">
          <div class="text-sm text-storm mt-3"><b>{{ $t('apps.env_xrandr_example') }}</b>
            <pre class="mt-1 text-silver bg-void p-2 rounded">sh -c "xrandr --output HDMI-1 --mode \"${POLARIS_CLIENT_WIDTH}x${POLARIS_CLIENT_HEIGHT}\" --rate ${POLARIS_CLIENT_FPS}"</pre>
          </div>
        </template>
        <template v-if="platform === 'macos'">
          <div class="text-sm text-storm mt-3"><b>{{ $t('apps.env_displayplacer_example') }}</b>
            <pre class="mt-1 text-silver bg-void p-2 rounded">sh -c "displayplacer "id:&lt;screenId&gt; res:${POLARIS_CLIENT_WIDTH}x${POLARIS_CLIENT_HEIGHT} hz:${POLARIS_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre>
          </div>
        </template>

        <div class="text-sm mt-2">
          <a href="https://docs.lizardbyte.dev/projects/polaris/latest/md_docs_2app__examples.html" target="_blank" class="text-ice hover:text-ice/80 no-underline">
            {{ $t('_common.see_more') }}
          </a>
        </div>
      </div>

      <div class="bg-twilight/50 border-l-4 border-blue-400 text-silver p-3 rounded-lg">
        <svg class="w-5 h-5 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
        {{ $t('apps.env_sunshine_compatibility') }}
      </div>

      <!-- Save buttons -->
      <div class="flex gap-2 pt-4">
        <button @click="showEditForm = false" class="inline-flex items-center gap-2 h-9 px-4 text-sm font-medium rounded-lg border border-storm text-silver hover:border-ice hover:text-ice hover:shadow-[0_0_16px_rgba(200,214,229,0.08)] transition-all duration-200">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12"/></svg>
          {{ $t('_common.cancel') }}
        </button>
        <button class="bg-ice text-void px-4 py-2 rounded-lg hover:bg-ice/80 transition disabled:opacity-50" :disabled="actionDisabled || !editForm.name.trim()" @click="save">
          <svg class="w-4 h-4 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4"/></svg>
          {{ $t('_common.save') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, inject } from 'vue'
import Checkbox from '../Checkbox.vue'
import { useToast } from '../composables/useToast'
import { useGameScanner } from '../composables/useGameScanner'

const { toast: showToast } = useToast()
const {
  scanning: gameScanning, importing: gameImporting,
  steamGames, lutrisGames, heroicGames, error: gameScanError,
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
const detachedCmd = ref("")
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
