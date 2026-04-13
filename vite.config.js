import { fileURLToPath, URL } from 'node:url'
import fs from 'fs';
import { resolve } from 'path'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from "vite-plugin-ejs";
import { codecovVitePlugin } from "@codecov/vite-plugin";
import tailwindcss from '@tailwindcss/vite'
import vue from '@vitejs/plugin-vue'
import process from 'process'

/**
 * Before actually building the pages with Vite, we do an intermediate build step using ejs
 * Importing this separately and joining them using ejs
 * allows us to split some repeating HTML that cannot be added
 * by Vue itself (e.g. style/script loading, common meta head tags, Widgetbot)
 * The vite-plugin-ejs handles this automatically
 */
let assetsSrcPath = 'src_assets/common/assets/web';
let assetsDstPath = 'build/assets/web';

if (process.env.POLARIS_BUILD_HOMEBREW || process.env.SUNSHINE_BUILD_HOMEBREW) {
    console.log("Building for homebrew, using default paths")
}
else {
    // If the paths supplied in the environment variables contain any symbolic links
    // at any point in the series of directories, the entire build will fail with
    // a cryptic error message like this:
    //     RollupError: The "fileName" or "name" properties of emitted chunks and assets
    //     must be strings that are neither absolute nor relative paths.
    // To avoid this, we resolve the potential symlinks using `fs.realpathSync` before
    // doing anything else with the paths.
    let sourceAssetsDir = process.env.POLARIS_SOURCE_ASSETS_DIR || process.env.SUNSHINE_SOURCE_ASSETS_DIR;
    let assetsDir = process.env.POLARIS_ASSETS_DIR || process.env.SUNSHINE_ASSETS_DIR;
    if (sourceAssetsDir) {
        let path = resolve(fs.realpathSync(sourceAssetsDir), "common/assets/web");
        console.log("Using srcdir from Cmake: " + path);
        assetsSrcPath = path;
    }
    if (assetsDir) {
        let path = resolve(fs.realpathSync(assetsDir), "assets/web");
        console.log("Using destdir from Cmake: " + path);
        assetsDstPath = path;
    }
}

let header = fs.readFileSync(resolve(assetsSrcPath, "template_header.html"))

// https://vitejs.dev/config/
export default defineConfig({
    resolve: {
        alias: {
            vue: 'vue/dist/vue.esm-bundler.js'
        }
    },
    base: './',
    plugins: [
        tailwindcss(),
        vue(),
        ViteEjsPlugin({ header }),
        // The Codecov vite plugin should be after all other plugins
        codecovVitePlugin({
            enableBundleAnalysis: process.env.CODECOV_TOKEN !== undefined,
            bundleName: "polaris",
            uploadToken: process.env.CODECOV_TOKEN,
        }),
    ],
    root: resolve(assetsSrcPath),
    build: {
        outDir: resolve(assetsDstPath),
        emptyOutDir: true,
        minify: 'esbuild',
        cssMinify: true,
        sourcemap: false,
        rollupOptions: {
            input: {
                index: resolve(assetsSrcPath, 'index.html'),
            },
        },
    },
})
