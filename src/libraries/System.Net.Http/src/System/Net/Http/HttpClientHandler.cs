// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Globalization;
using System.Net.Security;
using System.Collections.Generic;
using System.Runtime.Versioning;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics.Metrics;
#if TARGET_WASI
using System.Diagnostics;
using System.Net.Http.Metrics;
using HttpHandlerType = System.Net.Http.WasiHttpHandler;
#elif TARGET_BROWSER
using System.Diagnostics;
using System.Net.Http.Metrics;
using HttpHandlerType = System.Net.Http.BrowserHttpHandler;
#else
using HttpHandlerType = System.Net.Http.SocketsHttpHandler;
#endif

namespace System.Net.Http
{
    public partial class HttpClientHandler : HttpMessageHandler
    {
        private readonly HttpHandlerType _underlyingHandler;

#if TARGET_BROWSER || TARGET_WASI
        private IMeterFactory? _meterFactory;
        private HttpMessageHandler? _firstHandler; // DiagnosticsHandler or MetricsHandler, depending on global configuration.

        private HttpMessageHandler Handler
        {
            get
            {
                if (_firstHandler != null)
                {
                    return _firstHandler;
                }

                HttpMessageHandler handler = _underlyingHandler;

                // MetricsHandler should be descendant of DiagnosticsHandler in the handler chain to make sure the 'http.request.duration'
                // metric is recorded before stopping the request Activity. This is needed to make sure that our telemetry supports Exemplars.
                // Since HttpClientHandler.Proxy is unsupported on most platforms, don't bother passing it to telemetry handlers.
                if (GlobalHttpSettings.MetricsHandler.IsGloballyEnabled)
                {
                    handler = new MetricsHandler(handler, _meterFactory, proxy: null, out _);
                }
                if (GlobalHttpSettings.DiagnosticsHandler.EnableActivityPropagation)
                {
                    handler = new DiagnosticsHandler(handler, DistributedContextPropagator.Current, proxy: null);
                }

                // Ensure a single handler is used for all requests.
                if (Interlocked.CompareExchange(ref _firstHandler, handler, null) != null)
                {
                    handler.Dispose();
                }

                return _firstHandler;
            }
        }
#else
        private HttpHandlerType Handler => _underlyingHandler;
#endif

        private volatile bool _disposed;

        public HttpClientHandler()
        {
            _underlyingHandler = new HttpHandlerType();

            ClientCertificateOptions = ClientCertificateOption.Manual;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && !_disposed)
            {
                _disposed = true;
                _underlyingHandler.Dispose();
            }

            base.Dispose(disposing);
        }

        public virtual bool SupportsAutomaticDecompression => HttpHandlerType.SupportsAutomaticDecompression;
        public virtual bool SupportsProxy => HttpHandlerType.SupportsProxy;
        public virtual bool SupportsRedirectConfiguration => HttpHandlerType.SupportsRedirectConfiguration;

        /// <summary>
        /// Gets or sets the <see cref="IMeterFactory"/> to create a custom <see cref="Meter"/> for the <see cref="HttpClientHandler"/> instance.
        /// </summary>
        /// <remarks>
        /// When <see cref="MeterFactory"/> is set to a non-<see langword="null"/> value, all metrics emitted by the <see cref="HttpClientHandler"/> instance
        /// will be recorded using the <see cref="Meter"/> provided by the <see cref="IMeterFactory"/>.
        /// </remarks>
        [CLSCompliant(false)]
        public IMeterFactory? MeterFactory
        {
#if TARGET_BROWSER || TARGET_WASI
            get => _meterFactory;
            set
            {
                ObjectDisposedException.ThrowIf(_disposed, this);
                if (_firstHandler != null)
                {
                    throw new InvalidOperationException(SR.net_http_operation_started);
                }
                _meterFactory = value;
            }
#else
            get => _underlyingHandler.MeterFactory;
            set => _underlyingHandler.MeterFactory = value;
#endif
        }

        [UnsupportedOSPlatform("browser")]
        public bool UseCookies
        {
            get => _underlyingHandler.UseCookies;
            set => _underlyingHandler.UseCookies = value;
        }

        [UnsupportedOSPlatform("browser")]
        public CookieContainer CookieContainer
        {
            get => _underlyingHandler.CookieContainer;
            set
            {
                ArgumentNullException.ThrowIfNull(value);

                _underlyingHandler.CookieContainer = value;
            }
        }

        [UnsupportedOSPlatform("browser")]
        public DecompressionMethods AutomaticDecompression
        {
            get => _underlyingHandler.AutomaticDecompression;
            set => _underlyingHandler.AutomaticDecompression = value;
        }

        [UnsupportedOSPlatform("browser")]
        public bool UseProxy
        {
            get => _underlyingHandler.UseProxy;
            set => _underlyingHandler.UseProxy = value;
        }

        [UnsupportedOSPlatform("browser")]
        [UnsupportedOSPlatform("ios")]
        [UnsupportedOSPlatform("tvos")]
        public IWebProxy? Proxy
        {
            get => _underlyingHandler.Proxy;
            set => _underlyingHandler.Proxy = value;
        }

        [UnsupportedOSPlatform("browser")]
        public ICredentials? DefaultProxyCredentials
        {
            get => _underlyingHandler.DefaultProxyCredentials;
            set => _underlyingHandler.DefaultProxyCredentials = value;
        }

        [UnsupportedOSPlatform("browser")]
        public bool PreAuthenticate
        {
            get => _underlyingHandler.PreAuthenticate;
            set => _underlyingHandler.PreAuthenticate = value;
        }

        [UnsupportedOSPlatform("browser")]
        public bool UseDefaultCredentials
        {
            // SocketsHttpHandler doesn't have a separate UseDefaultCredentials property.  There
            // is just a Credentials property.  So, we need to map the behavior.
            get => _underlyingHandler.Credentials == CredentialCache.DefaultCredentials;
            set
            {
                if (value)
                {
                    _underlyingHandler.Credentials = CredentialCache.DefaultCredentials;
                }
                else
                {
                    if (_underlyingHandler.Credentials == CredentialCache.DefaultCredentials)
                    {
                        // Only clear out the Credentials property if it was a DefaultCredentials.
                        _underlyingHandler.Credentials = null;
                    }
                }
            }
        }

        [UnsupportedOSPlatform("browser")]
        public ICredentials? Credentials
        {
            get => _underlyingHandler.Credentials;
            set => _underlyingHandler.Credentials = value;
        }

        public bool AllowAutoRedirect
        {
            get => _underlyingHandler.AllowAutoRedirect;
            set => _underlyingHandler.AllowAutoRedirect = value;
        }

        [UnsupportedOSPlatform("browser")]
        public int MaxAutomaticRedirections
        {
            get => _underlyingHandler.MaxAutomaticRedirections;
            set => _underlyingHandler.MaxAutomaticRedirections = value;
        }

        [UnsupportedOSPlatform("browser")]
        public int MaxConnectionsPerServer
        {
            get => _underlyingHandler.MaxConnectionsPerServer;
            set => _underlyingHandler.MaxConnectionsPerServer = value;
        }

        public long MaxRequestContentBufferSize
        {
            // This property is not supported. In the .NET Framework it was only used when the handler needed to
            // automatically buffer the request content. That only happened if neither 'Content-Length' nor
            // 'Transfer-Encoding: chunked' request headers were specified. So, the handler thus needed to buffer
            // in the request content to determine its length and then would choose 'Content-Length' semantics when
            // POST'ing. In .NET Core, the handler will resolve the ambiguity by always choosing
            // 'Transfer-Encoding: chunked'. The handler will never automatically buffer in the request content.
            get
            {
                return 0; // Returning zero is appropriate since in .NET Framework it means no limit.
            }

            set
            {
                ArgumentOutOfRangeException.ThrowIfNegative(value);

                if (value > HttpContent.MaxBufferSize)
                {
                    throw new ArgumentOutOfRangeException(nameof(value), value,
                        SR.Format(CultureInfo.InvariantCulture, SR.net_http_content_buffersize_limit,
                        HttpContent.MaxBufferSize));
                }

                ObjectDisposedException.ThrowIf(_disposed, this);

                // No-op on property setter.
            }
        }

        [UnsupportedOSPlatform("browser")]
        public int MaxResponseHeadersLength
        {
            get => _underlyingHandler.MaxResponseHeadersLength;
            set => _underlyingHandler.MaxResponseHeadersLength = value;
        }

        public ClientCertificateOption ClientCertificateOptions
        {
            get => _underlyingHandler.ClientCertificateOptions;
            set
            {
                switch (value)
                {
                    case ClientCertificateOption.Manual:
#if !(TARGET_BROWSER || TARGET_WASI)
                        ThrowForModifiedManagedSslOptionsIfStarted();
                        _underlyingHandler.SslOptions.LocalCertificateSelectionCallback = (sender, targetHost, localCertificates, remoteCertificate, acceptableIssuers) => CertificateHelper.GetEligibleClientCertificate(_underlyingHandler.SslOptions.ClientCertificates)!;
#endif
                        break;

                    case ClientCertificateOption.Automatic:
#if !(TARGET_BROWSER || TARGET_WASI)
                        ThrowForModifiedManagedSslOptionsIfStarted();
                        _underlyingHandler.SslOptions.LocalCertificateSelectionCallback = (sender, targetHost, localCertificates, remoteCertificate, acceptableIssuers) => CertificateHelper.GetEligibleClientCertificate()!;
#endif
                        break;

                    default:
                        throw new ArgumentOutOfRangeException(nameof(value));
                }
                _underlyingHandler.ClientCertificateOptions = value;
            }
        }

        [UnsupportedOSPlatform("browser")]
        public X509CertificateCollection ClientCertificates
        {
            get
            {
                if (ClientCertificateOptions != ClientCertificateOption.Manual)
                {
                    throw new InvalidOperationException(SR.Format(SR.net_http_invalid_enable_first, nameof(ClientCertificateOptions), nameof(ClientCertificateOption.Manual)));
                }

                return _underlyingHandler.SslOptions.ClientCertificates ??
                    (_underlyingHandler.SslOptions.ClientCertificates = new X509CertificateCollection());
            }
        }

        [UnsupportedOSPlatform("browser")]
        public Func<HttpRequestMessage, X509Certificate2?, X509Chain?, SslPolicyErrors, bool>? ServerCertificateCustomValidationCallback
        {
#if TARGET_BROWSER || TARGET_WASI
            get => throw new PlatformNotSupportedException();
            set => throw new PlatformNotSupportedException();
#else
            get => (_underlyingHandler.SslOptions.RemoteCertificateValidationCallback?.Target as ConnectHelper.CertificateCallbackMapper)?.FromHttpClientHandler;
            set
            {
                ThrowForModifiedManagedSslOptionsIfStarted();
                _underlyingHandler.SslOptions.RemoteCertificateValidationCallback = value != null ?
                    new ConnectHelper.CertificateCallbackMapper(value).ForSocketsHttpHandler :
                    null;
            }
#endif
        }

        [UnsupportedOSPlatform("browser")]
        public bool CheckCertificateRevocationList
        {
            get => _underlyingHandler.SslOptions.CertificateRevocationCheckMode == X509RevocationMode.Online;
            set
            {
                ThrowForModifiedManagedSslOptionsIfStarted();
                _underlyingHandler.SslOptions.CertificateRevocationCheckMode = value ? X509RevocationMode.Online : X509RevocationMode.NoCheck;
            }
        }

        [UnsupportedOSPlatform("browser")]
        public SslProtocols SslProtocols
        {
            get => _underlyingHandler.SslOptions.EnabledSslProtocols;
            set
            {
                ThrowForModifiedManagedSslOptionsIfStarted();
                _underlyingHandler.SslOptions.EnabledSslProtocols = value;
            }
        }

        public IDictionary<string, object?> Properties => _underlyingHandler.Properties;

        //
        // Attributes are commented out due to https://github.com/dotnet/arcade/issues/7585
        // API compat will fail until this is fixed
        //
        //[UnsupportedOSPlatform("android")]
        [UnsupportedOSPlatform("browser")]
        //[UnsupportedOSPlatform("ios")]
        //[UnsupportedOSPlatform("tvos")]
        protected internal override HttpResponseMessage Send(HttpRequestMessage request, CancellationToken cancellationToken)
        {
#if TARGET_BROWSER || TARGET_WASI
            throw new PlatformNotSupportedException();
#else
            ArgumentNullException.ThrowIfNull(request);
            return Handler.Send(request, cancellationToken);
#endif
        }

        protected internal override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            ArgumentNullException.ThrowIfNull(request);
            return Handler.SendAsync(request, cancellationToken);
        }

        // lazy-load the validator func so it can be trimmed by the ILLinker if it isn't used.
        [UnsupportedOSPlatform("browser")]
        public static Func<HttpRequestMessage, X509Certificate2?, X509Chain?, SslPolicyErrors, bool> DangerousAcceptAnyServerCertificateValidator =>
            field ??
            Interlocked.CompareExchange(ref field, delegate { return true; }, null) ??
            field;

        private void ThrowForModifiedManagedSslOptionsIfStarted()
        {
            // Hack to trigger an InvalidOperationException if a property that's stored on
            // SslOptions is changed, since SslOptions itself does not do any such checks.
            _underlyingHandler.SslOptions = _underlyingHandler.SslOptions;
        }
    }
}
