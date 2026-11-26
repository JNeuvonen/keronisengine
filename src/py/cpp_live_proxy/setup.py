from setuptools import find_packages, setup

setup(
    name="cpp_live_proxy",
    version="0.1.0",
    packages=find_packages(),
    install_requires=[
        "fastapi>=0.115",
        "uvicorn[standard]>=0.30",
        "httpx>=0.27",
        "aiohttp>=3.10",
        "ccxt>=4.4",
        "pydantic>=2.8",
        "eth-account>=0.13",
        "py-clob-client>=0.25",
        "web3>=7.0",
    ],
)
