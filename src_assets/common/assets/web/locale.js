import {createI18n} from "vue-i18n";

async function loadConfiguredLocale() {
    try {
        const response = await fetch("./api/configLocale", { credentials: 'include' });
        const contentType = response.headers.get("content-type") || "";
        if (!response.ok || !contentType.includes("application/json")) {
            return "en";
        }

        const r = await response.json();
        return r.locale ?? "en";
    } catch (e) {
        console.error("Failed to load locale config", e);
        return "en";
    }
}

async function loadLocaleMessages(locale) {
    try {
        const response = await fetch(`./assets/locale/${locale}.json`, { credentials: 'include' });
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        return await response.json();
    } catch (e) {
        console.error("Failed to download translations", e);
        return {};
    }
}

export default async function() {
    let locale = await loadConfiguredLocale();
    document.querySelector('html').setAttribute('lang', locale);
    let messages = {
        en: await loadLocaleMessages('en')
    };
    if (locale !== 'en') {
        messages[locale] = await loadLocaleMessages(locale);
    }
    const i18n = createI18n({
        locale: locale, // set locale
        fallbackLocale: 'en', // set fallback locale
        messages: messages
    })
    return i18n;
}
