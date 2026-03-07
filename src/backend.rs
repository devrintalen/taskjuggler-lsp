use dashmap::DashMap;
use tower_lsp::jsonrpc::Result;
use tower_lsp::lsp_types::*;
use tower_lsp::{Client, LanguageServer};

use crate::parser::{self, ParseResult, Symbol};

pub struct Backend {
    client: Client,
    documents: DashMap<Url, ParseResult>,
}

impl Backend {
    pub fn new(client: Client) -> Self {
        Self {
            client,
            documents: DashMap::new(),
        }
    }

    async fn on_change(&self, uri: Url, text: String) {
        let result = parser::parse(&text);
        let diagnostics = result.diagnostics.clone();
        self.documents.insert(uri.clone(), result);
        self.client
            .publish_diagnostics(uri, diagnostics, None)
            .await;
    }
}

fn to_document_symbol(sym: &Symbol) -> DocumentSymbol {
    #[allow(deprecated)]
    DocumentSymbol {
        name: sym.name.clone(),
        detail: Some(sym.detail.clone()),
        kind: sym.kind,
        tags: None,
        deprecated: None,
        range: sym.range,
        selection_range: sym.selection_range,
        children: if sym.children.is_empty() {
            None
        } else {
            Some(sym.children.iter().map(to_document_symbol).collect())
        },
    }
}

#[tower_lsp::async_trait]
impl LanguageServer for Backend {
    async fn initialize(&self, _params: InitializeParams) -> Result<InitializeResult> {
        Ok(InitializeResult {
            capabilities: ServerCapabilities {
                text_document_sync: Some(TextDocumentSyncCapability::Kind(
                    TextDocumentSyncKind::FULL,
                )),
                document_symbol_provider: Some(OneOf::Left(true)),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    async fn initialized(&self, _params: InitializedParams) {
        self.client
            .log_message(MessageType::INFO, "taskjuggler-lsp initialized")
            .await;
    }

    async fn shutdown(&self) -> Result<()> {
        Ok(())
    }

    async fn did_open(&self, params: DidOpenTextDocumentParams) {
        self.on_change(params.text_document.uri, params.text_document.text)
            .await;
    }

    async fn did_change(&self, params: DidChangeTextDocumentParams) {
        if let Some(change) = params.content_changes.into_iter().last() {
            self.on_change(params.text_document.uri, change.text).await;
        }
    }

    async fn did_close(&self, params: DidCloseTextDocumentParams) {
        let uri = params.text_document.uri;
        self.documents.remove(&uri);
        self.client.publish_diagnostics(uri, vec![], None).await;
    }

    async fn document_symbol(
        &self,
        params: DocumentSymbolParams,
    ) -> Result<Option<DocumentSymbolResponse>> {
        let doc_symbols = self
            .documents
            .get(&params.text_document.uri)
            .map(|r| r.symbols.iter().map(to_document_symbol).collect::<Vec<_>>());
        Ok(doc_symbols.map(DocumentSymbolResponse::Nested))
    }
}
