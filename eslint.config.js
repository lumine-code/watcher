const js = require('@eslint/js');
const globals = require('globals');
module.exports = [
  {ignores: ['build/**', 'node_modules/**']},
  js.configs.recommended,
  {
    languageOptions: {sourceType: 'commonjs', globals: globals.node},
    rules: {
      'no-unused-vars': [
        'error',
        {varsIgnorePattern: '^_', argsIgnorePattern: '^_'},
      ],
    },
  },
];
