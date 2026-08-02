// The stylesheets are the ones the reference app shipped: losnoco.css carries the design tokens
// and base styles, app.css the control-surface layer on top of them. Ported wholesale so the two
// deployments of this page are the same page to look at.
import './css/losnoco.css';
import './css/app.css';

import { createApp } from 'vue';
import { createPinia } from 'pinia';
import App from './App.vue';
import { router } from './router';

createApp(App).use(createPinia()).use(router).mount('#app');
