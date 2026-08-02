import { createRouter, createWebHistory } from 'vue-router';
import PlayerPage from './pages/PlayerPage.vue';

export const router = createRouter({
    history: createWebHistory(),
    routes: [
        { path: '/', component: PlayerPage },
        // Lazy: the Live page carries the pickers and the keyboard, none of it needed to play a file.
        { path: '/live', component: () => import('./pages/LivePage.vue') },
    ],
});
