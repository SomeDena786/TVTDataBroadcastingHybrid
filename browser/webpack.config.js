const path = require('path');
const webpack = require('webpack')

module.exports = {
    entry: {
        TVTDataBroadcastingHybrid: './src/TVTDataBroadcastingHybrid.ts',
        aribhtml5_runtime: './src/aribhtml5_runtime.ts',
    },
    output: {
        path: path.resolve(__dirname, 'dist'),
        filename: '[name].js',
    },
    module: {
        rules: [
            {
                test: /\.ts$/,
                use: 'ts-loader',
            },
        ],
    },
    resolve: {
        extensions: [
            '.ts', '.js',
        ],
    },
};
