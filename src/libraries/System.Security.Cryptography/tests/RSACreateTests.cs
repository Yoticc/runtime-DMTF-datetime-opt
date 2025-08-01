// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Security.Cryptography.Rsa.Tests;
using Xunit;

namespace System.Security.Cryptography.Tests
{
    [SkipOnPlatform(TestPlatforms.Browser, "Not supported on Browser")]
    public static class RSACreateTests
    {
        // macOS has the highest smallest key value, 1024.
        // Windows CNG has the highest step size, 64.
        // This needs to take both into account.
        [Theory]
        [InlineData(1024)]
        [InlineData(1088)]
        [InlineData(1152)]
        [InlineData(2048)]
        public static void CreateWithKeysize(int keySizeInBits)
        {
            using (RSA rsa = RSA.Create(keySizeInBits))
            {
                Assert.Equal(keySizeInBits, rsa.KeySize);

                RSAParameters parameters = rsa.ExportParameters(false);
                Assert.Equal(keySizeInBits, parameters.Modulus.Length << 3);
                Assert.Equal(keySizeInBits, rsa.KeySize);
            }
        }

        [Theory]
        [InlineData(0)]
        [InlineData(1)]
        [InlineData(1023)]
        [InlineData(1025)]
        public static void CreateWithKeysize_InvalidKeySize(int keySizeInBits)
        {
            Assert.Throws<CryptographicException>(() => RSA.Create(keySizeInBits));
        }

        [Fact]
        public static void CreateWithParameters_1032()
        {
            CreateWithParameters(TestData.RSA1032Parameters);
        }

        [Fact]
        public static void CreateWithParameters_UnusualExponent()
        {
            CreateWithParameters(TestData.UnusualExponentParameters);
        }

        [Fact]
        public static void CreateWithParameters_2048()
        {
            CreateWithParameters(TestData.RSA2048Params);
        }

        private static void CreateWithParameters(RSAParameters parameters)
        {
            RSAParameters exportedPrivate;

            using (RSA rsa = RSA.Create(parameters))
            {
                exportedPrivate = rsa.ExportParameters(true);
            }

            RSATestHelpers.AssertKeyEquals(parameters, exportedPrivate);
        }

        [Fact]
        public static void CreateWithInvalidParameters()
        {
            RSAParameters parameters = TestData.RSA1032Parameters;
            parameters.Exponent = null;
            Assert.Throws<CryptographicException>(() => RSA.Create(parameters));
        }
    }
}
