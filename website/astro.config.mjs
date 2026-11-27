import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://engine.keronis.ai',
  integrations: [
    starlight({
      title: 'KeronisEngine',
      description:
        'Open-source C++20 trading engine built for predictive models, sub-millisecond ONNX inference, and live/backtest parity.',
      logo: {
        src: './src/assets/logo.svg',
      },
      social: [
        {
          icon: 'github',
          label: 'GitHub',
          href: 'https://github.com/JNeuvonen/keronisengine',
        },
      ],
      customCss: ['./src/styles/custom.css'],
      components: {
        Header: './src/components/StarlightHeader.astro',
        ThemeProvider: './src/components/ThemeProvider.astro',
        ThemeSelect: './src/components/EmptyThemeSelect.astro',
      },
      head: [
        {
          tag: 'link',
          attrs: {
            rel: 'preconnect',
            href: 'https://fonts.googleapis.com',
          },
        },
        {
          tag: 'link',
          attrs: {
            rel: 'preconnect',
            href: 'https://fonts.gstatic.com',
            crossorigin: true,
          },
        },
        {
          tag: 'link',
          attrs: {
            rel: 'stylesheet',
            href: 'https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&family=Inter:wght@400;500;600;700&display=swap',
          },
        },
        {
          tag: 'script',
          attrs: { type: 'module' },
          content: `
            const blocks = document.querySelectorAll('pre[data-language="mermaid"]');
            if (blocks.length) {
              const { default: mermaid } = await import('https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs');
              mermaid.initialize({ startOnLoad: false, theme: 'dark' });
              for (const pre of blocks) {
                const div = document.createElement('div');
                div.className = 'mermaid';
                div.textContent = pre.textContent || '';
                const fig = pre.closest('.expressive-code') || pre;
                fig.replaceWith(div);
              }
              await mermaid.run({ querySelector: '.mermaid' });
            }
          `,
        },
      ],
      sidebar: [
        {
          label: 'Getting Started',
          items: [
            { label: 'Overview', slug: 'docs' },
            { label: 'Quickstart', slug: 'quickstart' },
            { label: 'Comparison', slug: 'comparison' },
            { label: 'FAQ', slug: 'faq' },
          ],
        },
        {
          label: 'Core Concepts',
          items: [
            { label: 'Architecture', link: '/architecture/' },
            { label: 'Core Primitives & Invariants', slug: 'core-primitives' },
            { label: 'Strategies', slug: 'strategies' },
            { label: 'State Machines & ONNX', slug: 'state-machines' },
            { label: 'Data & Alt-Data', slug: 'data-and-alt-data' },
            { label: 'Custom Venues & Feeds', slug: 'custom-venues' },
          ],
        },
        {
          label: 'Operations & Reference',
          items: [
            { label: 'Live & Paper Trading', slug: 'live-and-paper' },
            { label: 'Control Server API', slug: 'control-server' },
            { label: 'Configuration', slug: 'configuration' },
          ],
        },
      ],
    }),
  ],
});
