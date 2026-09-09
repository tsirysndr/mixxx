//! `mixxx-mcp` — the MCP server an AI agent spawns to control Mixxx.
//!
//! With no arguments it speaks MCP on stdio, which is what agent hosts
//! expect. The other modes exist so a human can check the plumbing
//! without an agent in the loop.

use mixxx_mcp::client::Client;
use mixxx_mcp::{endpoint, mcp, tools};
use serde_json::Value;

const USAGE: &str = "\
mixxx-mcp — control Mixxx from an AI agent over the Model Context Protocol

USAGE:
    mixxx-mcp                     Speak MCP on stdio (what agent hosts run)
    mixxx-mcp --status            Show the discovered Mixxx endpoint and its state
    mixxx-mcp --list-tools        List the tools exposed to the agent
    mixxx-mcp --call M [PARAMS]   Call a mixxx.* method directly, PARAMS being JSON
    mixxx-mcp --version
    mixxx-mcp --help

Mixxx advertises its endpoint in mcp.json inside its settings directory;
set MIXXX_MCP_ENDPOINT to point at that file explicitly.";

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let exit = match args.first().map(String::as_str) {
        None => run_stdio().await,
        Some("--help" | "-h") => {
            println!("{USAGE}");
            0
        }
        Some("--version" | "-V") => {
            println!("mixxx-mcp {}", env!("CARGO_PKG_VERSION"));
            0
        }
        Some("--status") => status().await,
        Some("--list-tools") => list_tools(),
        Some("--call") => call(&args[1..]).await,
        Some(other) => {
            eprintln!("mixxx-mcp: unknown argument '{other}'\n\n{USAGE}");
            2
        }
    };
    std::process::exit(exit);
}

async fn run_stdio() -> i32 {
    match mcp::run().await {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("mixxx-mcp: {e}");
            1
        }
    }
}

async fn status() -> i32 {
    let Some((path, ep)) = endpoint::discover() else {
        eprintln!(
            "mixxx-mcp: no endpoint found. Looked in:\n{}",
            endpoint::default_endpoint_paths()
                .iter()
                .map(|p| format!("  {}", p.display()))
                .collect::<Vec<_>>()
                .join("\n")
        );
        return 1;
    };
    println!("endpoint file: {}", path.display());
    println!("url:           {}", ep.url());
    println!("mixxx pid:     {}", ep.pid);
    println!("rpc version:   {}", ep.rpc_version);

    match Client::discover().await {
        Ok(client) => match client.call("mixxx.get_state", serde_json::json!({})).await {
            Ok(state) => {
                println!(
                    "state:\n{}",
                    serde_json::to_string_pretty(&state).unwrap_or_default()
                );
                0
            }
            Err(e) => {
                eprintln!("connected, but mixxx.get_state failed: {e}");
                1
            }
        },
        Err(e) => {
            eprintln!("{e}");
            1
        }
    }
}

fn list_tools() -> i32 {
    for tool in tools::catalog() {
        println!("{:<24} -> {}", tool.name, tool.method);
    }
    0
}

async fn call(args: &[String]) -> i32 {
    let Some(method) = args.first() else {
        eprintln!("mixxx-mcp: --call needs a method name, e.g. --call mixxx.get_state");
        return 2;
    };
    let params: Value = match args.get(1) {
        Some(raw) => match serde_json::from_str(raw) {
            Ok(value) => value,
            Err(e) => {
                eprintln!("mixxx-mcp: params are not valid JSON: {e}");
                return 2;
            }
        },
        None => serde_json::json!({}),
    };
    let client = match Client::discover().await {
        Ok(client) => client,
        Err(e) => {
            eprintln!("mixxx-mcp: {e}");
            return 1;
        }
    };
    match client.call(method, params).await {
        Ok(result) => {
            println!(
                "{}",
                serde_json::to_string_pretty(&result).unwrap_or_default()
            );
            0
        }
        Err(e) => {
            eprintln!("mixxx-mcp: {e}");
            1
        }
    }
}
